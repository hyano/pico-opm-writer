/*
 * オンボード LED 駆動（非ブロッキング）
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LED_H
#define LED_H

typedef enum {
    LED_STATE_IDLE,    /* 待機中 */
    LED_STATE_CAPTURE, /* キャプチャ中 */
    LED_STATE_ERROR,   /* DMA overrun 等のエラー */
} led_state_t;

/* ---- API ---------------------------------------------------------------- */

void led_init(void);                    /* GPIO 初期化。以後 IDLE から始まる */
void led_set_state(led_state_t state);  /* 基本パターンの切り替え */
void led_notify_command(void);          /* コマンド受信の一時表示を差し込む */
void led_service(void);                 /* メインループから毎周回呼ぶ。時間が来たら LED を更新 */

#endif /* LED_H */
