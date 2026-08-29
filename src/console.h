/*
 * コマンドの受け口（CDC #0）
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef CONSOLE_H
#define CONSOLE_H

/*
 * 受信していれば 1 文字だけ取り込み、行が完成したらその場で実行する。
 *
 * **1 周回で 1 文字しか読まない。** シーケンサの予算（VGM_BUDGET_US /
 * MDX_BUDGET_US）がそのまま入力の受け取り速度の上限になるので、予算を
 * 大きくするとコマンド入力が目に見えて遅くなる（docs §9.2）。
 */
void console_service(void);

/*
 * ホストが CDC #0 を開いたか閉じたかを見る（内部で 100ms に間引く）。
 *
 * 開いた瞬間に起動バナーを出す。stdio_usb は開かれる前の出力を捨てるので、
 * 起動時に出しても読まれない。
 */
void console_poll_connect(void);

/*
 * 応答。1 コマンドにつきどちらかがちょうど 1 行出る（README §3.3「応答」）。
 * 情報行は `#` で始めることで、この規約を崩さずに非同期にも出せる。
 */
void reply_ok(void);
void reply_err(const char *reason);

#endif /* CONSOLE_H */
