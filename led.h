/*
 * オンボード LED 駆動（非ブロッキング）
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef LED_H
#define LED_H

#include <stdint.h>

typedef enum
{
    LED_STATE_IDLE,    /* 待機中 */
    LED_STATE_CAPTURE, /* キャプチャ中 */
    LED_STATE_ERROR,   /* DMA overrun 等のエラー */
    LED_STATE_HOST,    /* storage host 中（ファイルシステムを PC へ渡している） */
    LED_STATE_BOOT,    /* 起動時のモード選択。ボタンが離されるのを待っている */
} led_state_t;

/* ---- API ---------------------------------------------------------------- */

void led_init(void);                   /* GPIO 初期化。以後 IDLE から始まる */
void led_set_state(led_state_t state); /* 基本パターンの切り替え */
void led_notify_command(void);         /* コマンド受信の一時表示を差し込む */
void led_notify_button(void);          /* ボタンの長押し成立の一時表示を差し込む */
void led_service(void);                /* メインループから毎周回呼ぶ。時間が来たら LED を更新 */

/*
 * 起動時のモード選択を点滅回数で示す。状態を LED_STATE_BOOT にして、
 * blinks 回の点滅と休みを繰り返すパターンを選ぶ。blinks は 1-3。
 */
void led_boot_pattern(uint32_t blinks);

#endif /* LED_H */
