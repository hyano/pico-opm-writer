/*
 * ストレージのモード管理
 *
 * 内蔵フラッシュ後半のファイルシステムを、Pico 側（FatFs）と PC 側（USB MSC）の
 * どちらが持つかを排他で切り替える。同時に触らせないことが目的。
 *
 *   PLAYER : FatFs がマウント済み。MSC はメディア非挿入を返す。
 *   HOST   : FatFs はアンマウント。MSC がメディア挿入を返し PC から見える。
 *
 * HOST 中はフラッシュの消去が走るので、PCM キャプチャと I2S 出力を止める。
 * PLAYER へ戻すときに ym3012 と I2S のリング位置を張り直す。
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>
#include <stdint.h>

/* ---- モード ------------------------------------------------------------ */

typedef enum {
    /*
     * 0 にしておくと .bss の初期値が PLAYER = メディア非挿入になる。
     * tusb_init() から storage_init() までの隙間で MSC のコールバックが
     * 呼ばれても安全側に倒れる。
     */
    STORAGE_MODE_PLAYER = 0,
    STORAGE_MODE_HOST = 1,
} storage_mode_t;

/* ファイルシステムの状態。モードとは別に持つ。 */
typedef enum {
    STORAGE_FS_UNMOUNTED = 0, /* HOST 中 */
    STORAGE_FS_MOUNTED,
    STORAGE_FS_NO_FILESYSTEM, /* 未フォーマット */
    STORAGE_FS_IO_ERROR,
    STORAGE_FS_REGION_OVERLAP, /* 領域がファームウェアと重なっている */
} storage_fs_state_t;

/* ---- 初期化とサービス -------------------------------------------------- */

/*
 * 領域の妥当性を確かめ、PLAYER モードでマウントを試みる。
 * 未フォーマットでも自動でフォーマットはしない。
 */
void storage_init(void);

/*
 * メインループから毎周回呼ぶ。HOST モードでホストの書き込みが途切れたら、
 * dirty な行を 1 周回 1 つずつフラッシュへ書き出す。戻り値は実仕事をしたかどうか。
 */
bool storage_service(void);

/* ---- モードの切り替え -------------------------------------------------- */

/* 成功なら NULL、失敗ならエラー理由の文字列を返す（capture_start() と同じ流儀）。 */
const char *storage_set_host(void);
const char *storage_set_player(void);

/*
 * PC が SCSI の START_STOP UNIT で eject したときに呼ぶ。
 * ただしメディアを 1 度も読んでいないホストからの eject は無視する（下記）。
 */
void storage_host_ejected(void);

/* ---- 問い合わせ -------------------------------------------------------- */

storage_mode_t storage_mode(void);
const char *storage_mode_name(void);

storage_fs_state_t storage_fs_state(void);
const char *storage_fs_state_name(void);

/* MSC にメディアが入っていると見せるか。HOST モードでのみ true。 */
bool storage_medium_present(void);

/* FatFs 側からブロックデバイスを触ってよいか。PLAYER モードでのみ true。 */
bool storage_fatfs_may_access(void);

/* PC が書いたあと eject が来ていない */
bool storage_host_dirty(void);

/* ---- 情報 -------------------------------------------------------------- */

uint32_t storage_region_offset(void); /* XIP_BASE からのオフセット */
uint32_t storage_region_size(void);
uint32_t storage_firmware_end(void);  /* ファームウェア末尾のオフセット */

/* FAT の種類（"FAT12" など）。マウントしていなければ "-"。 */
const char *storage_fs_type_name(void);

/* クラスタ長 [byte]。マウントしていなければ 0。 */
uint32_t storage_cluster_bytes(void);

/* 空き / 全体 [KiB]。マウントしていなければ両方 0。 */
bool storage_space_kib(uint32_t *free_kib, uint32_t *total_kib);

/* ボリュームラベル。取れなければ空文字列。 */
const char *storage_label(void);

/* ---- フォーマット ------------------------------------------------------ */

/*
 * 領域を FAT12 / クラスタ 4096B / SFD で作り直し、/VGM を掘る。
 * 数十秒ブロックし、I2S アンダーランが何度も起きる。
 * 成功なら NULL、失敗ならエラー理由の文字列。
 */
const char *storage_format(void);

/* ---- MSC からの通知 ---------------------------------------------------- */

/*
 * HOST へ入った直後に 1 回だけ true を返す。
 *
 * メディアを入れ替えたことをホストへ知らせるための合図で、MSC 側はこれを見て
 * 1 回だけ UNIT ATTENTION（メディアが変わった）を返す。これをやらないと、
 * 一度 eject した PC は「取り外し済み」の状態を保持したままになり、
 * storage host に戻してもディスクとして現れない。
 */
bool storage_take_media_change(void);

/* PC がメディアを読んだ。実際に使い始めたかどうかの判定に使う。 */
void storage_note_host_read(void);

/* PC が書き込んだ。アイドル期限を張り直す。 */
void storage_note_host_write(void);

/* PC がキャッシュの同期を要求した。 */
bool storage_sync_now(void);

/* ---- MSC の診断 -------------------------------------------------------- */

void usb_msc_trace_reset(void);
void usb_msc_trace_dump(void);

#endif /* STORAGE_H */
