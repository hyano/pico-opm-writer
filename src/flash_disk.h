/*
 * 内蔵 QSPI フラッシュの一部をブロックデバイスとして扱う
 *
 * 論理セクタは 512 バイトで、FatFs と USB MSC の両方がこのサイズで見る。
 * 一方フラッシュの消去単位は 4096 バイトなので、書き込みは 4KiB 単位の
 * ライトバックキャッシュを挟む。
 *
 * 読み出しは XIP からの memcpy で済むのでブロックしない。書き込みだけが
 * 消去を伴い、そのあいだ IRQ が落ちてメインループが数十 ms 止まる。
 * この停止点を tud_task() の中に作らないため、キャッシュに載せる処理と
 * フラッシュへ書き出す処理を分けてある。
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FLASH_DISK_H
#define FLASH_DISK_H

#include <stdbool.h>
#include <stdint.h>

#include "hardware/flash.h"
#include "pico/stdlib.h"

/* ---- 領域 -------------------------------------------------------------- */

/*
 * 内蔵フラッシュ 4MiB のうち、ファームウェアの後ろから末尾の予約セクタまでを
 * ファイルシステムに使う。外付けフラッシュは使わないので GPIO は 1 本も
 * 消費しない。
 *
 * 通常は CMake の FLASH_FATFS_RESERVE_KB（ファームウェアに残す KiB）だけを
 * 指定すればよく、CMakeLists.txt が下の 3 つを計算して渡す。ここの #ifndef は
 * CMake を通さないビルドのための保険で、既定値は CMakeLists.txt と揃えてある。
 */

/*
 * 末尾に空けておくバイト数。picotool は UF2 に RP2350-E10 の回避用として
 * 0x10FFFF00 を狙う absolute block を入れる。4MiB のフラッシュではこの
 * アドレスが末尾セクタへ折り返すので、領域を末尾まで伸ばすと UF2 で焼くたびに
 * ファイルシステムの末尾 4KiB が潰れる。
 */
#ifndef FLASH_FATFS_TAIL_RESERVE
#define FLASH_FATFS_TAIL_RESERVE FLASH_SECTOR_SIZE
#endif

/* ファームウェアに残すバイト数 = 領域の開始位置（XIP_BASE からのオフセット） */
#ifndef FLASH_FATFS_OFFSET
#define FLASH_FATFS_OFFSET (256u * 1024u)
#endif

/* 開始位置から末尾の予約セクタの手前までを全部使う */
#ifndef FLASH_FATFS_SIZE
#define FLASH_FATFS_SIZE \
    (PICO_FLASH_SIZE_BYTES - FLASH_FATFS_TAIL_RESERVE - FLASH_FATFS_OFFSET)
#endif

/* ---- ジオメトリ -------------------------------------------------------- */

#define FLASH_DISK_SS 512u              /* 論理セクタ長 */
#define FLASH_DISK_ES FLASH_SECTOR_SIZE /* 消去単位 4096 = クラスタ長 */

#define FLASH_DISK_LBA_COUNT  (FLASH_FATFS_SIZE / FLASH_DISK_SS) /* 既定 7672 */
#define FLASH_DISK_LBA_PER_ES (FLASH_DISK_ES / FLASH_DISK_SS)    /* 8 */

/*
 * ライトバックキャッシュの行数。
 *
 * FAT12 のメタデータ（ブートセクタ + FAT + ルートディレクトリ）は 4KiB
 * ブロックにして 5 個ほどで、データクラスタとは別のブロックにある。PC の
 * ファイルコピーは FAT・ディレクトリ・データを交互に書くので、行が少ないと
 * そのたびに追い出しと消去が起きて消去回数が数倍になる。8 行あれば
 * メタデータが常駐し、4KiB 書き込みあたり消去 1 回に収まる。
 */
#define FLASH_DISK_CACHE_LINES 8u

/* ---- 戻り値 ------------------------------------------------------------ */

#define FLASH_DISK_OK   0 /* キャッシュに載せた（フラッシュにはまだ書いていない） */
#define FLASH_DISK_BUSY 1 /* 空き行が無く追い出す行が dirty。あとで再試行すること */
#define FLASH_DISK_ERR  (-1)

/* ---- API --------------------------------------------------------------- */

void flash_disk_init(void);

/*
 * 論理セクタ単位の読み出し。dirty な行が覆う範囲はその行から、それ以外は
 * XIP から memcpy する。ブロックしないし消去もしないし、キャッシュも汚さない。
 * 範囲外なら false。
 */
bool flash_disk_read(uint32_t lba, void *dst, uint32_t count);

/*
 * 論理セクタ単位の書き込み。キャッシュに載せるだけで、ここでは絶対に消去しない
 * （tud_task() の中から呼んでも安全にするため）。
 * 戻り値は FLASH_DISK_OK / BUSY / ERR。
 *
 * BUSY を返したとき、範囲の途中までキャッシュに載っていることがある。
 * 呼び出し側は同じ引数で再試行すること。同じ内容を書き直すだけなので冪等。
 */
int flash_disk_write(uint32_t lba, const void *src, uint32_t count);

/* 未書き出しの行数 */
uint32_t flash_disk_dirty_lines(void);

/*
 * 最も古い dirty 行を 1 つだけフラッシュへ書き出す。ここが唯一の停止点で、
 * 4KiB あたり典型 56ms、最悪 100ms 超のあいだ IRQ が落ちる。
 * 書き出し後に ym3012 と I2S のリング位置を張り直す。
 * dirty が無ければ false（何もしなかった）。失敗しても false。
 */
bool flash_disk_flush_one(void);

/* dirty が無くなるまで書き出す。失敗したら false。 */
bool flash_disk_flush_all(void);

/* 全行を無効化する。dirty が残っていると内容を捨てることになるので注意。 */
void flash_disk_invalidate(void);

/* 直前の書き出しでエラーが起きたか */
bool flash_disk_failed(void);

#endif /* FLASH_DISK_H */
