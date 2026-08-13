/*
 * オンボード LED 駆動の実装
 *
 * SPDX-License-Identifier: MIT
 */
#include "led.h"

#include "hardware/gpio.h"
#include "pico/stdlib.h"

/* ---- パターン定義（100ms スロット） ---------------------------------------- */

/*
 * 各パターンは "1"（ON） / "0"（OFF） の文字列。1 スロット = 100ms。
 * パターンの長さはスロット数。現在のスロット位置でビットを読み取り LED を更新する。
 */

/* 基本パターン：状態ごとの定義 */
static const char s_pattern_idle[] = "1";              /* 常時点灯 */
static const char s_pattern_capture[] = "1111100000";  /* 500ms ON / 500ms OFF */
static const char s_pattern_error[] = "1010100000000000"; /* 100ms ON/OFF x3 → 1s OFF */

/* オーバーレイ（コマンド受信） */
static const char s_pattern_command[] = "101";         /* 100ms ON / OFF / ON */

/* ---- 状態管理 ------------------------------------------------------------ */

static struct {
    led_state_t state;       /* 現在の基本状態 */
    uint32_t pattern_idx;    /* 基本パターンの現在スロット */
    uint32_t overlay_idx;    /* オーバーレイの現在スロット。-1 = 非アクティブ */
    uint32_t last_update_us; /* 最後に LED を更新した時刻 [us] */
    bool led_level;          /* LED の現在の状態（true = ON） */
} s_led = {
    .state = LED_STATE_IDLE,
    .pattern_idx = 0,
    .overlay_idx = (uint32_t)(-1), /* 非アクティブ = 最大値 */
    .last_update_us = 0,
    .led_level = false,
};

/* ---- パターン管理（文字列から状態へ） ------------------------------------- */

/*
 * 状態に応じた基本パターンへのポインタと長さを取得する。
 * pattern_idx の値が有効か確認済みの前提で呼び出す。
 */
static const char* pattern_for_state(led_state_t state, uint32_t *out_len) {
    switch (state) {
        case LED_STATE_IDLE:
            *out_len = sizeof(s_pattern_idle) - 1;
            return s_pattern_idle;
        case LED_STATE_CAPTURE:
            *out_len = sizeof(s_pattern_capture) - 1;
            return s_pattern_capture;
        case LED_STATE_ERROR:
            *out_len = sizeof(s_pattern_error) - 1;
            return s_pattern_error;
        default:
            *out_len = 0;
            return "";
    }
}

/* ---- 内部ヘルパー ------------------------------------------------------ */

/* LED を更新して、現在のスロット位置でビットを読み取る */
static void led_update_from_patterns(void) {
    uint32_t active_idx = s_led.pattern_idx;

    /* オーバーレイがアクティブなら優先する */
    if (s_led.overlay_idx < (uint32_t)sizeof(s_pattern_command) - 1) {
        active_idx = s_led.overlay_idx;
        /* パターン文字 '1' = ON（'0' = OFF） */
        s_led.led_level = (s_pattern_command[active_idx] == '1');
    } else {
        /* 基本パターンを使う */
        uint32_t pattern_len = 0;
        const char *pattern = pattern_for_state(s_led.state, &pattern_len);
        if (pattern_len > 0 && active_idx < pattern_len) {
            s_led.led_level = (pattern[active_idx] == '1');
        } else {
            s_led.led_level = false;
        }
    }

    /* 実際に LED へ反映 */
    gpio_put(PICO_DEFAULT_LED_PIN, s_led.led_level);
}

/* ---- API 実装 ------------------------------------------------------------ */

void led_init(void) {
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);

    /* 初期状態を IDLE で設定して LED を更新 */
    s_led.state = LED_STATE_IDLE;
    s_led.pattern_idx = 0;
    s_led.overlay_idx = (uint32_t)(-1);
    s_led.last_update_us = time_us_32();

    led_update_from_patterns();
}

void led_set_state(led_state_t state) {
    if (state != s_led.state) {
        /* 状態が変わったので位相をリセット */
        s_led.state = state;
        s_led.pattern_idx = 0;
        s_led.last_update_us = time_us_32();
        led_update_from_patterns();
    }
    /* 同じ状態を再設定した場合は位相を維持（無変更） */
}

void led_notify_command(void) {
    /* オーバーレイを先頭から再スタート */
    s_led.overlay_idx = 0;
    s_led.last_update_us = time_us_32();
    led_update_from_patterns();
}

void led_service(void) {
    uint32_t now_us = time_us_32();
    uint32_t elapsed_us = now_us - s_led.last_update_us;

    /* 100ms (100000us) 以上経過していなければ何もしない */
    if (elapsed_us < 100000u) {
        return;
    }

    /* スロット数を進める。複数スロット分の経過に対応。
     * 端数を切り捨てずに持ち越して、長時間動かしても位相がずれないようにする。 */
    uint32_t slots_advanced = elapsed_us / 100000u;
    s_led.last_update_us += slots_advanced * 100000u;

    /* オーバーレイがアクティブか判定 */
    if (s_led.overlay_idx < (uint32_t)sizeof(s_pattern_command) - 1) {
        /* オーバーレイの進行 */
        s_led.overlay_idx += slots_advanced;

        if (s_led.overlay_idx >= (uint32_t)sizeof(s_pattern_command) - 1) {
            /* オーバーレイが終了したので基本パターンを位相 0 から再開 */
            s_led.overlay_idx = (uint32_t)(-1);
            s_led.pattern_idx = 0;
        }
    } else {
        /* 基本パターンの進行 */
        uint32_t pattern_len = 0;
        pattern_for_state(s_led.state, &pattern_len);

        if (pattern_len > 0) {
            s_led.pattern_idx = (s_led.pattern_idx + slots_advanced) % pattern_len;
        }
    }

    led_update_from_patterns();
}
