/*
 * PCM キャプチャの状態機械
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef CAPTURE_H
#define CAPTURE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * キャプチャの状態。
 *
 *   IDLE --p 1--> CAPTURING --p 0--> DRAINING --送り切り--> IDLE
 *                      |                  |
 *                 CDC#1 切断 / overrun   CDC#1 切断
 *                      +---> IDLE / ERROR <---+
 *
 * PIO と DMA はこの状態に関係なく常時動いている。ここで切り替えているのは
 * 「リングから読み出して CDC #1 へ流すかどうか」だけ。
 */
typedef enum
{
    CAPTURE_STATE_IDLE,      /* 送信していない */
    CAPTURE_STATE_CAPTURING, /* 送信中 */
    CAPTURE_STATE_DRAINING,  /* p 0 時点までの残りを送り切っている最中 */
    CAPTURE_STATE_ERROR,     /* DMA overrun。p 1 が来るまで止まる */
} capture_state_t;

void capture_init(void);

/*
 * メインループから毎周回呼ぶ。リングの位置更新・統計・overrun 判定と、
 * 1 回分の PCM 送信を行う。
 *
 * 戻り値は「CPU が実際に仕事をしたか」。USB の空き待ちで何も送れなかった場合は
 * false を返すので、呼び出し側はその周回を idle として扱える。
 */
bool capture_service(void);

/*
 * リング一周 65.5ms を超えてメインループが止まったあとに呼ぶ
 * （フラッシュの消去・書き込みなど）。
 *
 * 3 つのリングは DMA ポインタの差分をリング長で剰余を取って積んでいるので、
 * 一周を超えて止まると位置を見失う。張り直しを 1 本にまとめてあるのは、
 * 消費者が増えたときに呼び忘れが起きないようにするため。
 */
void capture_resync_after_blackout(void);

/* p 1 相当。成功なら NULL、失敗ならエラー理由の文字列を返す。 */
const char *capture_start(void);

/*
 * p 0 相当。DRAINING へ移すだけで、送り切りはメインループが進める。
 * 成功なら NULL、失敗ならエラー理由の文字列を返す。
 */
const char *capture_request_stop(void);

/* ドレインを打ち切って IDLE へ戻す（タイムアウト時） */
void capture_abort(void);

capture_state_t capture_state(void);
const char *capture_state_name(void);

#endif /* CAPTURE_H */
