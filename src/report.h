/*
 * `i` / `s` / `h` の状態表示
 *
 * どれも本文を出すだけで、末尾の OK は出さない。「1 コマンド 1 応答」
 * （README §3.3「応答」）を守るのは呼び出し側（console.c）の責務。
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef REPORT_H
#define REPORT_H

void report_info(void);  /* `i`。起動バナーとしても使う */
void report_stats(void); /* `s`（引数なし）。実行時統計 */
void report_help(void);  /* `h` / `?` / `help` */

#endif /* REPORT_H */
