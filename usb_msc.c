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
#include <string.h>

#include "tusb.h"

#include "flash_disk.h"
#include "storage.h"

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
}

/*
 * メディアが入っているか。false のとき TinyUSB が自分で
 * MEDIUM NOT PRESENT の sense を立てるので、こちらで触る必要はない。
 * PLAYER モードでは非挿入にすることで、PC からの SCSI コマンドが
 * read10 / write10 に到達する前にすべて失敗する。
 */
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    return storage_medium_present();
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_count = FLASH_DISK_LBA_COUNT;
    *block_size = (uint16_t)FLASH_DISK_SS;
}

/* PLAYER モードでは書き込み禁止として見せる */
bool tud_msc_is_writable_cb(uint8_t lun) {
    (void)lun;
    return storage_medium_present();
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

    if (load_eject && !start) {
        storage_host_ejected();
    }
    return true;
}

/* macOS の eject を通すために許可しておく */
bool tud_msc_prevent_allow_medium_removal_cb(uint8_t lun, uint8_t prohibit_removal,
                                             uint8_t control) {
    (void)lun;
    (void)prohibit_removal;
    (void)control;
    return true;
}
