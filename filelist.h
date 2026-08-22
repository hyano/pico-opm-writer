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

#endif /* FILELIST_H */
