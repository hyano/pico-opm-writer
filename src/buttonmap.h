/*
 * ボタン起点の操作（GP21 / GP22 を何に割り当てるか）
 *
 * 取り込みは button.h、割り当ては ここ、と分けてある。button.c は autoplay も
 * storage も知らず、GPIO を短押し / 長押しのイベントに変えるところで止まる
 * （docs §13.1）。
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BUTTONMAP_H
#define BUTTONMAP_H

/*
 * メールボックスに溜まったイベントを 1 個消化する。
 *
 * **必ず `main()` の `for(;;)` 直下から呼ぶ。** `service_all()` の中から呼ぶと、
 * コマンド処理中の待ちから再入して `filelist_collect()` の走査バッファを壊し、
 * コマンドの応答の途中へ別の出力が割り込む。ここが `process_line()` の外側で
 * あることが構文的に保証される唯一の点（docs §1.1）。
 */
void buttonmap_dispatch(void);

/*
 * 起動時に押されていたボタンの組み合わせで動作モードを選ぶ。
 * 初期化列をすべて終えてから 1 回だけ呼ぶ（autoplay_start() が storage の
 * マウントを前提にするため）。
 */
void buttonmap_boot_apply(void);

/*
 * 上で選んだ動作モードの結果の文字列（`i` の button 行に出す）。
 * 起動直後の printf はホストが CDC を開く前なので捨てられるため、後から
 * USB を挿しても分かるようにここへ控えてある。通常起動なら "none"。
 */
const char *buttonmap_boot_result(void);

#endif /* BUTTONMAP_H */
