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
 *   IDLE --p 2--> WAITING --play--> CAPTURING --曲の終わり--> DRAINING --> IDLE
 *                      |                  |
 *                 CDC#1 切断 / overrun   CDC#1 切断
 *                      +---> IDLE / ERROR <---+
 *
 * PIO と DMA はこの状態に関係なく常時動いている。ここで切り替えているのは
 * 「リングから読み出して CDC #1 へ流すかどうか」だけ。
 *
 * `p 1` と `p 2` の違いは**録り始めと録り終わりだけ**で、送り先も PCM の形式も
 * ドレインの手順も overrun の条件も同じ。`p 2` は次の演奏が始まるまで WAITING で
 * 待ち、その曲が終わって余韻も消えたところで自分から止まる。
 */
typedef enum
{
    CAPTURE_STATE_IDLE,      /* 送信していない */
    CAPTURE_STATE_WAITING,   /* p 2。次の演奏が始まるのを待っている（まだ送らない） */
    CAPTURE_STATE_CAPTURING, /* 送信中 */
    CAPTURE_STATE_DRAINING,  /* 送り切っている最中 */
    CAPTURE_STATE_ERROR,     /* DMA overrun。次の p 1 / p 2（または p 0）まで止まる */
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

/*
 * p 1 / p 2 相当。成功なら NULL、失敗ならエラー理由の文字列を返す。
 *
 * auto_stop なら WAITING から始まり、**次の**演奏で録り始めてその曲の終わりで止まる。
 * track_seq には呼んだ時点の通し番号を渡す（これが「次」の基準になる。鳴っている
 * 最中に打っても、その曲は録らずに次の play を待つ）。auto_stop が false なら見ない。
 */
const char *capture_start(bool auto_stop, uint32_t track_seq);

/*
 * メインループから毎周回渡す「いま曲が続いているか」と「何曲目か」。
 *
 * active は演奏中と余韻待ちの両方を含む（songend_is_active()）。seq は演奏を
 * 始めた回数で、変わったら新しい曲。**capture.c は vgm / mdx / songend を知らない。**
 * 判定を持ち込むと、storage の排他をコマンド層へ置いたのと同じ理由で層が濁る。
 */
void capture_note_track(bool active, uint32_t seq);

/*
 * p 0 相当。DRAINING へ移すだけで、送り切りはメインループが進める。
 * 成功なら NULL、失敗ならエラー理由の文字列を返す。
 */
const char *capture_request_stop(void);

/* ドレインを打ち切って IDLE へ戻す（タイムアウト時） */
void capture_abort(void);

capture_state_t capture_state(void);
const char *capture_state_name(void);
bool capture_is_auto(void); /* p 2 で始めた（曲の終わりで自分から止まる）か */

#endif /* CAPTURE_H */
