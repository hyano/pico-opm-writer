/*
 * gzip ストリームの展開
 *
 * FatFs の FIL から gzip ストリームを読み、展開したバイト列を前から順に返す。
 * 一時ファイルは作らず、展開結果は 32KB のリングにしか置かない。
 * VGM の知識は持たない。
 *
 * 展開器は miniz の tinfl（external/miniz、MIT）。
 *
 * gzip は後方シークできないので、ループ再生のために「展開器の状態を丸ごと
 * 保存して戻す」機構を持つ。tinfl_decompressor はポインタを含まない POD で、
 * 出力リングは呼び出しごとに渡す設計なので、構造体とリングの memcpy だけで
 * 完全に復元できる。
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VGZ_H
#define VGZ_H

#include <stdbool.h>
#include <stdint.h>

#include "ff.h"

#ifndef VGM_VGZ_ENABLED
#define VGM_VGZ_ENABLED 1
#endif

/* 展開後サイズが不明なときに vgz_isize() が返す値 */
#define VGZ_SIZE_UNKNOWN 0xFFFFFFFFu

/* ---- 開閉 -------------------------------------------------------------- */

/*
 * fp を gzip ストリームとして開く。fp は呼び出し側が f_open 済みで、
 * 閉じるのも呼び出し側。位置は問わない（内部で先頭へシークする）。
 *
 * gzip ヘッダの検証まで同期的に行う。成功なら true。
 */
bool vgz_open(FIL *fp);

/* 参照している FIL を手放す。fp を閉じるのは呼び出し側。 */
void vgz_close(void);

/* ---- 読み出し ---------------------------------------------------------- */

/*
 * 展開したバイト列を dst へ最大 n バイト書く。返り値は実際に書けたバイト数で、
 * n 未満なら終端かエラー（vgz_error() で区別する）。
 */
uint32_t vgz_read(void *dst, uint32_t n);

/* 展開ストリームが壊れていたか */
bool vgz_error(void);

/* 展開後の現在位置 */
uint32_t vgz_tell(void);

/*
 * gzip トレーラの ISIZE（展開後サイズ）。信用できなければ VGZ_SIZE_UNKNOWN。
 * 複数メンバの連結や 4GB 超では正しくないが、VGM の配布物は単一メンバ。
 */
uint32_t vgz_isize(void);

/* ---- ループ用のスナップショット ---------------------------------------- */

/*
 * 現在位置の展開器の状態を保存する。保存できるのは 1 箇所だけで、
 * 呼ぶたびに上書きされる。
 */
void vgz_snapshot(void);

/* 保存した状態へ戻す。保存が無ければ false。 */
bool vgz_restore(void);

bool vgz_snapshot_valid(void);

/*
 * ストリームを先頭から開き直す。スナップショットが取れなかったときの保険。
 * スナップショットは破棄しない。
 */
bool vgz_rewind(void);

#endif /* VGZ_H */
