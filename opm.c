/*
 * OPM (YM2151) バス制御の実装
 *
 * SPDX-License-Identifier: MIT
 */
#include "opm.h"

#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/platform.h"

#include "opm_clock.pio.h"

/* GPIO マスク */
#define OPM_MASK_DATA ((uint32_t)0xffu << OPM_PIN_D0)
#define OPM_MASK_A0   ((uint32_t)1u << OPM_PIN_A0)
#define OPM_MASK_WR   ((uint32_t)1u << OPM_PIN_WR)
#define OPM_MASK_IC   ((uint32_t)1u << OPM_PIN_IC)
#define OPM_MASK_LED  ((uint32_t)1u << OPM_PIN_LED)

#define OPM_MASK_ALL (OPM_MASK_DATA | OPM_MASK_A0 | OPM_MASK_WR | OPM_MASK_IC | OPM_MASK_LED)

/* φM 生成に使っている PIO / ステートマシン */
static PIO s_pio;
static uint s_sm;
static uint s_offset;

/* PIO 分周比（16.8 固定小数）と、そこから逆算した実 φM 周波数 */
static uint32_t s_div_int;
static uint32_t s_div_frac;
static uint32_t s_clock_hz_actual;

/* OPM_T_SETUP_NS 相当のシステムクロックサイクル数 */
static uint32_t s_setup_cycles;

/* LED のアクティビティ表示用の状態 */
static bool s_led_state;

/*
 * φM の分周比を算出して PIO を起動する。
 *
 * φM = sys_clk / (2 x div) なので div = sys_clk / (2 x φM)。
 * これを 1/256 刻みの固定小数にすると div256 = sys_clk x 128 / φM。
 */
static void opm_clock_start(void) {
    uint32_t sys_hz = clock_get_hz(clk_sys);

    uint64_t div256 = ((uint64_t)sys_hz * 128u + OPM_CLOCK_HZ / 2u) / OPM_CLOCK_HZ;
    if (div256 < 256u) {
        div256 = 256u; /* 分周比 1 未満にはできない */
    }

    s_div_int = (uint32_t)(div256 >> 8);
    s_div_frac = (uint32_t)(div256 & 0xffu);
    s_clock_hz_actual = (uint32_t)(((uint64_t)sys_hz * 128u) / div256);

    bool ok = pio_claim_free_sm_and_add_program(&opm_clock_program, &s_pio, &s_sm, &s_offset);
    hard_assert(ok);

    opm_clock_program_init(s_pio, s_sm, s_offset, OPM_PIN_PHIM, s_div_int, (uint8_t)s_div_frac);
    pio_sm_set_enabled(s_pio, s_sm, true);
}

/* データバスと A0 を確定させてから /WR を 1 回叩く（1 バスサイクル） */
static void opm_bus_cycle(bool a0, uint8_t value) {
    gpio_put_masked(OPM_MASK_DATA | OPM_MASK_A0,
                    ((uint32_t)value << OPM_PIN_D0) | (a0 ? OPM_MASK_A0 : 0u));
    busy_wait_at_least_cycles(s_setup_cycles); /* t_SETUP */

    gpio_clr_mask(OPM_MASK_WR);
    busy_wait_us_32(OPM_T_WR_US);
    gpio_set_mask(OPM_MASK_WR);
}

void opm_init(void) {
    /* 出力ピンをまとめて初期化。/WR と /IC は負論理なので H から始める。 */
    gpio_init_mask(OPM_MASK_ALL);
    gpio_put_masked(OPM_MASK_ALL, OPM_MASK_WR | OPM_MASK_IC);
    gpio_set_dir_out_masked(OPM_MASK_ALL);

    s_led_state = false;

    /* ns 指定のセットアップ時間をサイクル数へ（切り上げ） */
    uint32_t sys_hz = clock_get_hz(clk_sys);
    s_setup_cycles =
        (uint32_t)(((uint64_t)sys_hz * OPM_T_SETUP_NS + 999999999u) / 1000000000u);

    /* /IC を操作する前に φM を動かしておく必要がある */
    opm_clock_start();

    opm_reset();
}

void opm_write(uint8_t addr, uint8_t data) {
    /* アドレスサイクル */
    opm_bus_cycle(false, addr);
    busy_wait_us_32(OPM_T_ADDR_US);

    /* データサイクル。/RD 未接続で BUSY を読めないため固定時間待つ。 */
    opm_bus_cycle(true, data);
    busy_wait_us_32(OPM_T_DATA_US);

    s_led_state = !s_led_state;
    gpio_put(OPM_PIN_LED, s_led_state);
}

void opm_reset(void) {
    gpio_clr_mask(OPM_MASK_IC);
    sleep_ms(OPM_T_IC_LOW_MS);
    gpio_set_mask(OPM_MASK_IC);
    sleep_ms(OPM_T_IC_WAIT_MS);
}

void opm_clear(void) {
    /* 1. 全 8 チャンネルを KEY OFF */
    for (uint8_t ch = 0; ch < 8; ch++) {
        opm_write(0x08, ch);
    }

    /* 2. 全 32 スロットの TL を最小音量に */
    for (uint8_t reg = 0x60; reg <= 0x7f; reg++) {
        opm_write(reg, 0x7f);
    }

    opm_write(0x0f, 0x00); /* 3. ノイズ off */
    opm_write(0x14, 0x00); /* 4. タイマ / IRQ 停止 */
    opm_write(0x01, 0x00); /* 5. LFO リセット解除 */
    opm_write(0x18, 0x00); /* 6. LFO 周波数 */

    /* 7. 0x19 は bit7 で書き込み先が切り替わる。PMD(0x80) と AMD(0x00) の両方を消す。 */
    opm_write(0x19, 0x80);
    opm_write(0x19, 0x00);

    opm_write(0x1b, 0x00); /* 8. LFO 波形 / CT1・CT2 */

    /* 9. RL / FB / CONNECT */
    for (uint8_t reg = 0x20; reg <= 0x27; reg++) {
        opm_write(reg, 0x00);
    }
}

uint32_t opm_clock_hz_actual(void) {
    return s_clock_hz_actual;
}

uint32_t opm_clock_div_int(void) {
    return s_div_int;
}

uint32_t opm_clock_div_frac(void) {
    return s_div_frac;
}
