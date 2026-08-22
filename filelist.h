/*
 * FatFs 上のファイル一覧の出力（VGM / MDX で共用）
 *
 * `vgm list` と `mdx list` は、ディレクトリ・拡張子・件数上限を除いて同じ処理を
 * していたので 1 本にまとめてある。出力の書式もここが唯一の持ち場。
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef FILELIST_H
#define FILELIST_H

#include <stdbool.h>
#include <stdint.h>

/*
 * 大小無視の名前比較。大小無視で等しいときは strcmp() で決める。
 *
 * 同値で 0 を返さないのは、一覧が「直前より大きいものの中で最小」を繰り返して
 * 並べる方式で、順序が一意に決まらないと同じ名前を出し続けるか取りこぼすため。
 */
int filelist_name_cmp(const char *a, const char *b);

/*
 * dir の中の、exts のいずれかで終わる通常ファイルを名前の昇順で 1 行ずつ出力する。
 *
 * exts は ".vgm" のように先頭のドットを含めた文字列の配列で、比較は大小無視。
 * 拡張子の前に 1 文字以上を要求するので、".vgm" という名前そのものは一致しない。
 * 隠しファイル・システム属性・ディレクトリと、macOS が作る先頭ドットの
 * AppleDouble は除く。
 *
 * tick は 1 行出すごとに呼ぶ（NULL 可）。1 行の出力で USB が詰まらないよう、
 * 呼び出し側のサービス関数を渡すために用意してある。
 *
 * 戻り値は成功なら NULL、失敗ならエラー理由の文字列。
 */
const char *filelist_print(const char *dir, const char *const *exts, uint32_t n_exts,
                           uint32_t max_entries, void (*tick)(void));

/* ---- 一覧を RAM へ集める ----------------------------------------------- */

/*
 * filelist_collect() の作業領域。バッファは呼び出し側が用意する。
 *
 * filelist_print() が RAM を使わない代わりに 1 件ごとにディレクトリを全走査するのに対し、
 * こちらは 1 回の走査で名前をバッファへ詰める。任意の位置の名前を後から引ける必要が
 * ある用途（autoplay のプレイリスト）向け。
 */
typedef struct {
    char *pool;          /* 名前を '\0' 区切りで詰めるバッファ */
    uint32_t pool_bytes; /* pool の大きさ */
    uint32_t pool_used;  /* 詰まったバイト数 */
    uint16_t *offs;      /* 名前の昇順に並んだ pool へのオフセット */
    uint32_t max_entries;/* offs の要素数 */
    uint32_t count;      /* 詰まった件数 */
    bool truncated;      /* 上限に当たって打ち切ったか */
} filelist_buf_t;

/* buf を空にする。pool / pool_bytes / offs / max_entries は呼び出し側が先に埋めておく。 */
void filelist_buf_reset(filelist_buf_t *buf);

/*
 * dir の中の対象ファイルを buf へ集める。対象の選び方は filelist_print() と同じ。
 *
 * **buf->count はリセットせず追記し、今回足した範囲だけを名前の昇順に並べる。**
 * 続けて別のディレクトリを集めれば「A の昇順 → B の昇順」が 1 個のバッファに作れる。
 *
 * name_max より長い名前は飛ばす（呼び出し側がそれ以上の名前を扱えないため）。
 * 上限に当たったら buf->truncated を立てて打ち切る。
 *
 * 戻り値は成功なら NULL、失敗ならエラー理由の文字列。
 *
 * **filelist_print() と作業用の FILINFO を共用しているので再入できない。**
 * どちらも tick から自分自身へ戻ってこない経路で使うこと。
 */
const char *filelist_collect(const char *dir, const char *const *exts, uint32_t n_exts,
                             uint32_t name_max, filelist_buf_t *buf);

#endif /* FILELIST_H */
