/*
 * USB マスストレージ（内蔵フラッシュ後半を PC へ見せる）
 *
 * ディスクリプタには常に MSC が入っている。storage host / storage player は
 * インタフェースの付け外しではなく「メディアの挿抜」として表現するので、
 * モードを切り替えても USB の再列挙は起きず CDC #0 / #1 は切れない。
 *
 * ここのコールバックはすべて tud_task() の中から呼ばれる。読み出しは XIP からの
 * memcpy なのでブロックしない。書き込みはキャッシュに載せるだけで済むことが多いが、
 * 空きが無ければその場でフラッシュへ 1 行書き出す（数十 ms ブロックする）。
 * HOST モードでは PCM キャプチャと I2S を止めてあるので、この停止に巻き込む相手はいない。
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "tusb.h"

#include "flash_disk.h"
#include "storage.h"

/* ---- SCSI トレース（診断用） -------------------------------------------- */

/*
 * ホストが投げた SCSI をリングに控える。コールバックは tud_task() の中から
 * 呼ばれるので、ここで printf すると stdio_usb が tud_task() を再入しうる。
 * 記録だけして、表示は storage trace から行う。
 */
#define MSC_TRACE_MAX 320u

typedef struct {
    uint8_t ev;
    uint8_t a;
    uint8_t b;
    uint8_t c;
} msc_trace_t;

static msc_trace_t s_trace[MSC_TRACE_MAX];
static uint32_t s_trace_n; /* 総数。MSC_TRACE_MAX を超えたら古いものから捨てる */

static void trace(uint8_t ev, uint8_t a, uint8_t b, uint8_t c) {
    msc_trace_t *t = &s_trace[s_trace_n % MSC_TRACE_MAX];
    t->ev = ev;
    t->a = a;
    t->b = b;
    t->c = c;
    s_trace_n++;
}

void usb_msc_trace_reset(void) {
    s_trace_n = 0;
}

void usb_msc_trace_dump(void) {
    uint32_t total = s_trace_n;
    uint32_t shown = (total < MSC_TRACE_MAX) ? total : MSC_TRACE_MAX;
    uint32_t first = total - shown;
    printf("# msc     : %u events (showing %u)\n", (unsigned)total, (unsigned)shown);

    /* R が連続するところは 1 行にまとめる（ホストの読み込みで埋まるため） */
    uint32_t i = 0;
    while (i < shown) {
        const msc_trace_t *t = &s_trace[(first + i) % MSC_TRACE_MAX];
        if (t->ev == 'R') {
            uint32_t j = i;
            while (j < shown && s_trace[(first + j) % MSC_TRACE_MAX].ev == 'R') {
                j++;
            }
            printf("# msc %03u : R x%u\n", (unsigned)(first + i), (unsigned)(j - i));
            i = j;
            continue;
        }
        printf("# msc %03u : %c %02x %02x %02x\n", (unsigned)(first + i), t->ev, t->a,
               t->b, t->c);
        i++;
    }
}

/* 書き出しても空きが作れないとき、無限に回らないようにする上限 */
#define MSC_BUSY_RETRY_MAX 16u

/* TinyUSB の msc.h には無いので自前で置く */
#define SCSI_CMD_SYNCHRONIZE_CACHE_10 0x35

/* ---- 問い合わせ -------------------------------------------------------- */

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8], uint8_t product_id[16],
                        uint8_t product_rev[4]) {
    (void)lun;

    /* SCSI の規定どおり空白詰めの固定長。終端は入れない。 */
    memcpy(vendor_id, "PicoOPM ", 8);
    memcpy(product_id, "VGM Storage     ", 16);
    memcpy(product_rev, "0001", 4);
    trace('I', 0, 0, 0);
}

/*
 * メディアが入っているか。false のとき TinyUSB が自分で
 * MEDIUM NOT PRESENT の sense を立てるので、こちらで触る必要はない。
 * PLAYER モードでは非挿入にすることで、PC からの SCSI コマンドが
 * read10 / write10 に到達する前にすべて失敗する。
 */
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    if (!storage_medium_present()) {
        trace('T', 0, 0, 0);
        return false;
    }

    /*
     * HOST へ入った直後の 1 回だけ、メディアが入れ替わったことを通知する。
     * false を返すと CHECK CONDITION になるが、sense を先に立てておけば
     * TinyUSB は MEDIUM NOT PRESENT で上書きせずこちらを返す
     * （msc_device.c は sense_key が 0 のときだけ既定値を入れる）。
     *
     * これが無いと、一度 eject した PC は「取り外し済み」の状態を保持し、
     * storage host に戻してもディスクとして現れない。
     */
    if (storage_take_media_change()) {
        tud_msc_set_sense(lun, SCSI_SENSE_UNIT_ATTENTION, 0x28, 0x00);
        trace('T', 1, 1, 0);
        return false;
    }

    trace('T', 1, 0, 1);
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_count = FLASH_DISK_LBA_COUNT;
    *block_size = (uint16_t)FLASH_DISK_SS;
    trace('C', 0, 0, 0);
}

/* PLAYER モードでは書き込み禁止として見せる */
bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    bool w = storage_medium_present();
    trace('W', w ? 1u : 0u, 0, 0);
    return w;
}

/* ---- 読み書き ---------------------------------------------------------- */

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset, void *buffer,
                          uint32_t bufsize) {
    (void)lun;

    if (!storage_medium_present()) {
        return -1;
    }
    /* CFG_TUD_MSC_EP_BUFSIZE == 論理セクタ長なので offset は常に 0 になる */
    if (offset != 0u || (bufsize % FLASH_DISK_SS) != 0u) {
        return -1;
    }

    /*
     * dirty な行があればそこから、無ければ XIP から memcpy するだけ。
     * ブロックしないしキャッシュも汚さないので、PC からの読み出し（マウント・
     * ディレクトリ閲覧・コピーアウト）では消去が 1 度も起きない。
     */
    if (!flash_disk_read(lba, buffer, bufsize / FLASH_DISK_SS)) {
        return -1;
    }

    storage_note_host_read();
    trace('R', (uint8_t)(lba >> 8), (uint8_t)lba, (uint8_t)(bufsize / 512u));
    return (int32_t)bufsize;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset, uint8_t *buffer,
                           uint32_t bufsize) {
    (void)lun;

    if (!storage_medium_present()) {
        return -1;
    }
    if (offset != 0u || (bufsize % FLASH_DISK_SS) != 0u) {
        return -1;
    }

    /*
     * キャッシュが埋まっていたら、その場で 1 行書き出して空きを作る。
     *
     * 0 を返して「あとで呼び直してもらう」方式は使えない。tud_task() は
     * イベントキューを空になるまで同じ呼び出しの中で回すので、0 を返しても
     * メインループには戻らず、storage_service() が走る隙が無い（実測: 再試行が
     * 2944 回空回りしてコピーが止まった）。
     *
     * ここで数十 ms ブロックするが、HOST モードでは PCM キャプチャと I2S を
     * 止めてあるので巻き込む相手がいない。flash_disk_flush_one() が書き出しの
     * 直後にリング位置を張り直すので、キャプチャ側の総フレーム数もずれない。
     */
    for (uint32_t retry = 0; retry < MSC_BUSY_RETRY_MAX; retry++) {
        int rc = flash_disk_write(lba, buffer, bufsize / FLASH_DISK_SS);

        if (rc == FLASH_DISK_OK) {
            storage_note_host_write();
            return (int32_t)bufsize;
        }
        if (rc != FLASH_DISK_BUSY) {
            return -1;
        }
        if (!flash_disk_flush_one()) {
            return -1;
        }
    }

    return -1;
}

void tud_msc_write10_complete_cb(uint8_t lun) {
    (void)lun;
    storage_note_host_write(); /* アイドル期限を張り直す */
}

/* ---- その他の SCSI ----------------------------------------------------- */

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16], void *buffer,
                        uint16_t bufsize) {
    (void)buffer;
    (void)bufsize;

    trace('X', scsi_cmd[0], scsi_cmd[1], (uint8_t)bufsize);

    switch (scsi_cmd[0]) {
    case SCSI_CMD_SYNCHRONIZE_CACHE_10:
        /* 実際の書き出しは storage_sync_now() の中の判断に任せる */
        if (!storage_sync_now()) {
            return -1;
        }
        return 0;

    default:
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        return -1;
    }
}

/*
 * PC 側の eject。ここで所有権を Pico へ返す。PC の「取り出し」がそのまま
 * storage player の代わりになる。
 */
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start,
                           bool load_eject) {
    (void)lun;
    (void)power_condition;

    trace('S', power_condition, start ? 1u : 0u, load_eject ? 1u : 0u);

    if (load_eject && !start) {
        storage_host_ejected();
    }
    return true;
}

/* macOS の eject を通すために許可しておく */
bool tud_msc_prevent_allow_medium_removal_cb(uint8_t lun, uint8_t prohibit_removal,
                                             uint8_t control) {
    (void)lun;
    (void)control;
    trace('P', prohibit_removal, 0, 0);
    return true;
}
