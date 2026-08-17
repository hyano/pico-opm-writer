/*
 * ストレージのモード管理の実装
 *
 * SPDX-License-Identifier: MIT
 */
#include "storage.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "ff.h"

#include "capture.h"
#include "flash_disk.h"
#include "i2s.h"
#include "vgm.h"
#include "ym3012.h"

/* リンカが置くファームウェア末尾。領域の重なり検査に使う。 */
extern char __flash_binary_end;

/* PC が書いてから、まだ書き出していない行を寝かせておく時間 */
#define STORAGE_FLUSH_IDLE_MS 250u

/* ---- 状態 -------------------------------------------------------------- */

static storage_mode_t s_mode;
static storage_fs_state_t s_fs_state;

static FATFS s_fs;
static uint32_t s_fw_end;

static bool s_host_dirty; /* PC が書いたが eject が来ていない */
static bool s_media_change; /* HOST へ入った直後の 1 回だけ立てる */
static bool s_host_used;    /* PC がメディアを読んだか */
static absolute_time_t s_flush_deadline;

static char s_label[16];

/* f_mkfs の作業領域。大きいほど速いので消去単位ぶん取る。 */
static uint8_t s_mkfs_work[FLASH_DISK_ES];

/* ---- マウント ---------------------------------------------------------- */

static void mount_now(void) {
    FRESULT fr = f_mount(&s_fs, "", 1);

    switch (fr) {
    case FR_OK:
        s_fs_state = STORAGE_FS_MOUNTED;
        break;
    case FR_NO_FILESYSTEM:
        s_fs_state = STORAGE_FS_NO_FILESYSTEM;
        break;
    default:
        s_fs_state = STORAGE_FS_IO_ERROR;
        break;
    }

    s_label[0] = '\0';
    if (fr == FR_OK) {
        if (f_getlabel("", s_label, NULL) != FR_OK) {
            s_label[0] = '\0';
        }
    }
}

/* ---- 初期化 ------------------------------------------------------------ */

void storage_init(void) {
    flash_disk_init();

    s_mode = STORAGE_MODE_PLAYER;
    s_host_dirty = false;
    s_flush_deadline = get_absolute_time();
    s_label[0] = '\0';

    s_fw_end = (uint32_t)((uintptr_t)&__flash_binary_end - XIP_BASE);

    /*
     * ファームウェアが領域に食い込んでいたら、マウントも書き込みも一切しない。
     * hard_assert で止めると基板が起動しなくなって復旧しづらいので、
     * 状態として持ち回り i / storage status から見えるようにする。
     */
    if (s_fw_end > FLASH_FATFS_OFFSET) {
        s_fs_state = STORAGE_FS_REGION_OVERLAP;
        printf("# ERR storage region overlaps firmware (end 0x%08x, region 0x%08x)\n",
               (unsigned)(XIP_BASE + s_fw_end), (unsigned)(XIP_BASE + FLASH_FATFS_OFFSET));
        return;
    }

    mount_now();
}

/* ---- 毎周回の処理 ------------------------------------------------------ */

bool storage_service(void) {
    if (s_mode != STORAGE_MODE_HOST) {
        return false;
    }

    uint32_t dirty = flash_disk_dirty_lines();
    if (dirty == 0u) {
        return false;
    }

    /*
     * 書き出す条件は 2 つだけ。
     *
     * ホストからの書き込みが STORAGE_FLUSH_IDLE_MS 途切れたら（コピーが終わったら）
     * dirty が無くなるまで 1 周回 1 行で流し切る。それだけ。
     *
     * キャッシュが埋まったときの書き出しは tud_msc_write10_cb() がその場で行う。
     * ここで先回りして書き出すことはしない。ホストは同じブロック（FAT や
     * ディレクトリ）を何度も書き直すので、先回りすると書き出したそばから汚れ直し、
     * 書き込み回数が数倍になる（実測: 256KiB のコピーで 640 回 vs 必要なのは約 210 回）。
     *
     * SCSI の SYNCHRONIZE CACHE でも書き出さない。macOS (fskit) はコピー中に
     * これを頻繁に投げてくるため。確実に書き切るのは eject と storage player の
     * ときで、そこでは全部書き出す。
     */
    if (!time_reached(s_flush_deadline)) {
        return false;
    }

    if (!flash_disk_flush_one()) {
        s_fs_state = STORAGE_FS_IO_ERROR;
        printf("# ERR storage io error\n");
        return true;
    }

    /*
     * 期限切れで始まった書き出しは、期限を過去のままにしておくことで
     * 次の周回も続き、dirty が無くなるまで 1 周回 1 行で流し切る。
     * 途中でホストが書き始めれば storage_note_host_write() が期限を張り直すので
     * 自然に中断され、中途半端なブロックを何度も書かずに済む。
     */
    return true;
}

/* ---- モードの切り替え -------------------------------------------------- */

const char *storage_set_host(void) {
    if (s_fs_state == STORAGE_FS_REGION_OVERLAP) {
        return "no filesystem";
    }
    if (s_mode == STORAGE_MODE_HOST) {
        return NULL; /* 冪等 */
    }

    /*
     * HOST に渡すとファイルシステムをアンマウントするので、再生中の VGM が
     * 読めなくなる。キャプチャも消去の停止に巻き込まれる。どちらも先に止めさせる。
     */
    if (vgm_is_playing()) {
        printf("# hint    : VGM 再生中は切り替えられない。先に vgm stop を実行すること\n");
        return "wrong state";
    }
    if (capture_state() != CAPTURE_STATE_IDLE) {
        printf("# hint    : PCM キャプチャ中は切り替えられない。先に p 0 を実行すること\n");
        return "wrong state";
    }

    /* format 直後などで dirty が残っていれば先に片付ける */
    if (!flash_disk_flush_all()) {
        s_fs_state = STORAGE_FS_IO_ERROR;
        return "io error";
    }

    /*
     * PC がフラッシュを書いているあいだは消去で数十 ms 止まる。キャプチャと
     * I2S を巻き込まないよう、音声経路を切ってから所有権を渡す。
     * BCK / LRCK と DMA は止めない（止めると DAC がポップする）。
     */
    i2s_set_enabled(false);

    f_unmount("");
    flash_disk_invalidate();

    s_fs_state = STORAGE_FS_UNMOUNTED;
    s_label[0] = '\0';
    s_host_dirty = false;
    s_media_change = true;
    s_host_used = false;
    s_flush_deadline = make_timeout_time_ms(STORAGE_FLUSH_IDLE_MS);
    s_mode = STORAGE_MODE_HOST;

    return NULL;
}

const char *storage_set_player(void) {
    if (s_fs_state == STORAGE_FS_REGION_OVERLAP) {
        return "no filesystem";
    }
    if (s_mode == STORAGE_MODE_PLAYER) {
        return NULL; /* 冪等 */
    }

    /* PC が ACK 済みの分はすべて確定させてから所有権を取り戻す */
    bool flushed = flash_disk_flush_all();

    s_mode = STORAGE_MODE_PLAYER;
    flash_disk_invalidate();

    if (!flushed) {
        s_fs_state = STORAGE_FS_IO_ERROR;
        i2s_set_enabled(true);
        ym3012_ring_resync();
        i2s_resync();
        return "io error";
    }

    mount_now();

    /*
     * HOST 中はフラッシュの消去でリング一周 65.5ms を超えて止まっているので、
     * ym3012 と I2S の総フレーム数にずれが溜まっている。ここで断ち切る。
     */
    i2s_set_enabled(true);
    ym3012_ring_resync();
    i2s_resync();

    if (s_host_dirty) {
        printf("# warn    : ホストが eject していません。ファイルが不完全な可能性があります\n");
    }
    s_host_dirty = false;

    return NULL;
}

void storage_host_ejected(void) {
    if (s_mode != STORAGE_MODE_HOST) {
        return;
    }

    /*
     * メディアを 1 度も読んでいないホストからの eject は、利用者の操作ではなく
     * 「前に取り外したのだから入れ直すな」という macOS の指示。これに従うと
     * 一度 eject したあと storage host に戻せなくなるので無視する。
     * 本物の eject は必ずマウント（= 読み出し）のあとに来る。
     */
    if (!s_host_used) {
        return;
    }

    s_host_dirty = false; /* eject は正規の手順なので警告は出さない */
    (void)storage_set_player();
    printf("# storage : ejected by host\n");
}

/* ---- MSC からの通知 ---------------------------------------------------- */

bool storage_take_media_change(void) {
    if (!s_media_change) {
        return false;
    }
    s_media_change = false;
    return true;
}

void storage_note_host_read(void) {
    s_host_used = true;
}

void storage_note_host_write(void) {
    s_host_used = true;
    s_host_dirty = true;
    s_flush_deadline = make_timeout_time_ms(STORAGE_FLUSH_IDLE_MS);
}

bool storage_sync_now(void) {
    /*
     * ここで全部書き出さないのは意図的。macOS はコピー中に何度もこれを投げるので、
     * 素直に従うと消去回数が 1 桁増える（storage_service() のコメント参照）。
     * 書き込みと同じくアイドル期限を張り直すだけにして、実際の書き出しは
     * storage_service() のアイドル判定に任せる。
     *
     * ACK 済みのデータは RAM のキャッシュに載っており、eject と storage player で
     * 必ず全部フラッシュへ落とす。失うとすればその前に電源が切れた場合だけで、
     * これは書き戻しキャッシュを持つ USB メモリと同じ性質。
     */
    s_flush_deadline = make_timeout_time_ms(STORAGE_FLUSH_IDLE_MS);
    return true;
}

/* ---- 問い合わせ -------------------------------------------------------- */

storage_mode_t storage_mode(void) {
    return s_mode;
}

const char *storage_mode_name(void) {
    return (s_mode == STORAGE_MODE_HOST) ? "HOST" : "PLAYER";
}

storage_fs_state_t storage_fs_state(void) {
    return s_fs_state;
}

const char *storage_fs_state_name(void) {
    switch (s_fs_state) {
    case STORAGE_FS_MOUNTED:
        return "mounted";
    case STORAGE_FS_NO_FILESYSTEM:
        return "no filesystem";
    case STORAGE_FS_IO_ERROR:
        return "io error";
    case STORAGE_FS_REGION_OVERLAP:
        return "region overlaps firmware";
    case STORAGE_FS_UNMOUNTED:
    default:
        return "unmounted";
    }
}

bool storage_medium_present(void) {
    return s_mode == STORAGE_MODE_HOST && s_fs_state != STORAGE_FS_REGION_OVERLAP;
}

bool storage_fatfs_may_access(void) {
    return s_mode == STORAGE_MODE_PLAYER && s_fs_state != STORAGE_FS_REGION_OVERLAP;
}

bool storage_host_dirty(void) {
    return s_host_dirty;
}

/* ---- 情報 -------------------------------------------------------------- */

uint32_t storage_region_offset(void) {
    return FLASH_FATFS_OFFSET;
}

uint32_t storage_region_size(void) {
    return FLASH_FATFS_SIZE;
}

uint32_t storage_firmware_end(void) {
    return s_fw_end;
}

const char *storage_fs_type_name(void) {
    if (s_fs_state != STORAGE_FS_MOUNTED) {
        return "-";
    }
    switch (s_fs.fs_type) {
    case FS_FAT12:
        return "FAT12";
    case FS_FAT16:
        return "FAT16";
    case FS_FAT32:
        return "FAT32";
    default:
        return "?";
    }
}

uint32_t storage_cluster_bytes(void) {
    if (s_fs_state != STORAGE_FS_MOUNTED) {
        return 0u;
    }
    return (uint32_t)s_fs.csize * FLASH_DISK_SS;
}

bool storage_space_kib(uint32_t *free_kib, uint32_t *total_kib) {
    *free_kib = 0u;
    *total_kib = 0u;

    if (s_fs_state != STORAGE_FS_MOUNTED) {
        return false;
    }

    FATFS *fs = NULL;
    DWORD free_clusters = 0;
    if (f_getfree("", &free_clusters, &fs) != FR_OK || fs == NULL) {
        return false;
    }

    uint32_t cluster_kib_num = (uint32_t)fs->csize * FLASH_DISK_SS;
    *free_kib = (uint32_t)(((uint64_t)free_clusters * cluster_kib_num) / 1024u);
    *total_kib =
        (uint32_t)(((uint64_t)(fs->n_fatent - 2u) * cluster_kib_num) / 1024u);
    return true;
}

const char *storage_label(void) {
    return s_label;
}

/* ---- フォーマット ------------------------------------------------------ */

const char *storage_format(void) {
    if (s_fs_state == STORAGE_FS_REGION_OVERLAP) {
        return "no filesystem";
    }
    if (s_mode != STORAGE_MODE_PLAYER) {
        return "wrong state";
    }

    /*
     * クラスタ長を消去単位と同じ 4096 バイトにするのが最重要。
     * 2MiB / 4KiB ≒ 505 クラスタなので FAT12 が選ばれる（FAT16 にするには
     * 4085 クラスタ以上必要で、そのためにクラスタを 512 バイトへ落とすと
     * クラスタと消去セクタの 1:1 対応が壊れる）。
     * SFD = MBR 無し。USB リムーバブルメディアの標準的な見せ方。
     */
    static const MKFS_PARM opt = {
        .fmt = FM_FAT | FM_SFD,
        .n_fat = 1,      /* 2MiB では冗長性より容量と消去回数を取る */
        .align = 0,      /* disk_ioctl(GET_BLOCK_SIZE) から自動で決める */
        .n_root = 512,   /* 16KiB。LFN は 1 名で複数スロット使うので余裕を持たせる */
        .au_size = FLASH_DISK_ES,
    };

    /* 作り直す前にマウントを外す */
    f_unmount("");
    s_fs_state = STORAGE_FS_UNMOUNTED;

    FRESULT fr = f_mkfs("", &opt, s_mkfs_work, sizeof(s_mkfs_work));
    if (fr != FR_OK) {
        flash_disk_invalidate();
        mount_now();
        return "io error";
    }

    if (!flash_disk_flush_all()) {
        s_fs_state = STORAGE_FS_IO_ERROR;
        return "io error";
    }

    mount_now();
    if (s_fs_state != STORAGE_FS_MOUNTED) {
        return "io error";
    }

    if (f_setlabel("OPMVGM") != FR_OK) {
        return "io error";
    }

    if (f_mkdir("/VGM") != FR_OK) {
        return "io error";
    }

    /*
     * macOS の Spotlight にインデックスさせない。2MiB しかない領域を
     * .Spotlight-V100 や .fseventsd で食われるのを防ぐ。
     */
    FIL fp;
    if (f_open(&fp, "/.metadata_never_index", FA_CREATE_ALWAYS | FA_WRITE) == FR_OK) {
        f_close(&fp);
    }

    if (!flash_disk_flush_all()) {
        s_fs_state = STORAGE_FS_IO_ERROR;
        return "io error";
    }

    /* ラベルを読み直す */
    mount_now();

    return NULL;
}
