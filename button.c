/*
 * GPIO ボタンの取り込みの実装
 *
 * SPDX-License-Identifier: MIT
 */
#include "button.h"

#if BUTTON_ENABLED

#include "hardware/gpio.h"
#include "pico/stdlib.h"

#include "led.h"

/* ---- エピソードの状態 -------------------------------------------------- */

/*
 * エピソード = 最初の押下から全解放まで。chord = そのエピソード中に押された
 * ボタンの OR。離すときのズレ（SW1 を離してから SW2 を離すまでの数十 ms）は、
 * 全解放までエピソードが続くので自動的に吸収される。
 */
typedef enum
{
    EP_IDLE = 0, /* 何も押されていない */
    EP_ACTIVE,   /* 押されている。まだ長押しは成立していない */
    EP_LOCKED,   /* 長押しが成立した。全解放まで何もしない */
} ep_state_t;

static struct
{
    /* デバウンス。ビット位置は BUTTON_MASK_* と同じ */
    uint32_t raw_last;     /* 最後に読んだ生値 */
    uint32_t raw_since_us; /* その生値になった時刻 */
    uint32_t stable;       /* 確定値 */

    ep_state_t ep;
    uint32_t chord;
    uint32_t hold_start_us; /* 最後に chord が広がった時刻 */

    uint32_t boot_chord; /* button_init() の時点で押されていた集合 */

    bool box_full;
    button_event_t box;
    uint32_t dropped;
} s_btn;

/* ---- 内部ヘルパー ------------------------------------------------------ */

/* 生値を読む。負論理なので L = 押下。 */
static uint32_t read_raw(void)
{
    uint32_t mask = 0u;

    if (!gpio_get(BUTTON_PIN_SW1))
    {
        mask |= BUTTON_MASK_SW1;
    }
    if (!gpio_get(BUTTON_PIN_SW2))
    {
        mask |= BUTTON_MASK_SW2;
    }
    return mask;
}

/*
 * デバウンス。2 本まとめて 1 つの窓で見る。
 *
 * 片方が暴れている間はもう片方も確定しないが、確定値は保持されたままなので
 * 実害が無い。ピンごとに窓を持つより状態が少なくて済む。
 */
static void debounce(uint32_t now_us)
{
    uint32_t raw = read_raw();

    if (raw != s_btn.raw_last)
    {
        s_btn.raw_last = raw;
        s_btn.raw_since_us = now_us;
        return;
    }
    if (raw != s_btn.stable && (uint32_t)(now_us - s_btn.raw_since_us) >= BUTTON_DEBOUNCE_US)
    {
        s_btn.stable = raw;
    }
}

/* メールボックスへ置く。埋まっていたら捨てる（古い方を残す）。 */
static void post(button_press_t press, uint32_t mask)
{
    if (s_btn.box_full)
    {
        s_btn.dropped++;
        return;
    }
    s_btn.box.press = press;
    s_btn.box.mask = mask;
    s_btn.box_full = true;
}

/* 確定値を見てエピソードを進める */
static void episode(uint32_t now_us)
{
    uint32_t held = s_btn.stable;

    switch (s_btn.ep)
    {
    case EP_IDLE:
        if (held != 0u)
        {
            s_btn.chord = held;
            s_btn.hold_start_us = now_us;
            s_btn.ep = EP_ACTIVE;
        }
        break;

    case EP_ACTIVE:
        if (held == 0u)
        {
            /* しきい値未満で離した */
            post(BUTTON_PRESS_SHORT, s_btn.chord);
            s_btn.ep = EP_IDLE;
            s_btn.chord = 0u;
            break;
        }
        if ((held | s_btn.chord) != s_btn.chord)
        {
            /*
             * chord が広がったのでホールドを張り直す。
             *
             * これをやらないと、SW1 を単独で 0.9 秒押しているところへ SW2 が
             * 0.05 秒遅れて入った瞬間に chord が BOTH になり、1.0 秒の時点で
             * storage host が誤爆する。起点を「最後に chord が広がった時刻」に
             * すれば、和音のズレを吸収する追加の定数も要らない。
             */
            s_btn.chord |= held;
            s_btn.hold_start_us = now_us;
            break;
        }
        if ((uint32_t)(now_us - s_btn.hold_start_us) >= BUTTON_LONG_PRESS_US)
        {
            /* 離すのを待たずにここで成立させ、LED で合図する */
            post(BUTTON_PRESS_LONG, s_btn.chord);
            led_notify_button();
            s_btn.ep = EP_LOCKED;
        }
        break;

    case EP_LOCKED:
    default:
        if (held == 0u)
        {
            s_btn.ep = EP_IDLE;
            s_btn.chord = 0u;
        }
        break;
    }
}

/* ---- API 実装 ---------------------------------------------------------- */

void button_init(void)
{
    gpio_init(BUTTON_PIN_SW1);
    gpio_set_dir(BUTTON_PIN_SW1, GPIO_IN);
    gpio_pull_up(BUTTON_PIN_SW1);

    gpio_init(BUTTON_PIN_SW2);
    gpio_set_dir(BUTTON_PIN_SW2, GPIO_IN);
    gpio_pull_up(BUTTON_PIN_SW2);

    /*
     * プルアップの整定は RC で µs オーダーだが当てにせず、同じ値が
     * BUTTON_DEBOUNCE_US 続くのを確かめてから起動時の chord を確定する。
     * ここはまだ PCM を何も送っていないので、10ms 止まっても誰も困らない。
     */
    uint32_t raw = read_raw();
    uint32_t since_us = time_us_32();
    while ((uint32_t)(time_us_32() - since_us) < BUTTON_DEBOUNCE_US)
    {
        uint32_t now_raw = read_raw();
        if (now_raw != raw)
        {
            raw = now_raw;
            since_us = time_us_32();
        }
    }

    s_btn.raw_last = raw;
    s_btn.raw_since_us = time_us_32();
    s_btn.stable = raw;
    s_btn.boot_chord = raw;

    /*
     * 起動時に押されていたぶんは起動モードで消化するので、実行時のイベントには
     * しない。EP_LOCKED から始めれば、離した時点で短押しを出さずに IDLE へ戻る。
     */
    s_btn.ep = (raw != 0u) ? EP_LOCKED : EP_IDLE;
    s_btn.chord = raw;
    s_btn.hold_start_us = s_btn.raw_since_us;
    s_btn.box_full = false;
    s_btn.dropped = 0u;
}

void button_service(void)
{
    uint32_t now_us = time_us_32();

    debounce(now_us);
    episode(now_us);
}

uint32_t button_boot_chord(void)
{
    return s_btn.boot_chord;
}

void button_boot_wait_release(void (*tick)(void))
{
    while (s_btn.stable != 0u)
    {
        if (tick != NULL)
        {
            tick();
        }
        button_service();
    }

    /* 起動時の押下が実行時のイベントとして残らないようにする */
    s_btn.ep = EP_IDLE;
    s_btn.chord = 0u;
    s_btn.box_full = false;
}

bool button_take_event(button_event_t *out)
{
    if (!s_btn.box_full)
    {
        return false;
    }
    if (out != NULL)
    {
        *out = s_btn.box;
    }
    s_btn.box_full = false;
    return true;
}

uint32_t button_dropped(void)
{
    return s_btn.dropped;
}

#else /* !BUTTON_ENABLED */

/*
 * ビルド時に落とした場合。GP21 / GP22 には一切触らないので、ピンを他の用途へ
 * 回せる。呼び出し側に #if を持ち込まないよう、API は全部残して空にする。
 */

void button_init(void)
{
}

void button_service(void)
{
}

uint32_t button_boot_chord(void)
{
    return 0u;
}

void button_boot_wait_release(void (*tick)(void))
{
    (void)tick;
}

bool button_take_event(button_event_t *out)
{
    (void)out;
    return false;
}

uint32_t button_dropped(void)
{
    return 0u;
}

#endif /* BUTTON_ENABLED */
