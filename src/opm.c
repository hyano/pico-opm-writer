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
#include "stats.h"

/* GPIO マスク */
#define OPM_MASK_DATA ((uint32_t)0xffu << OPM_PIN_D0)
#define OPM_MASK_A0   ((uint32_t)1u << OPM_PIN_A0)
#define OPM_MASK_CS   ((uint32_t)1u << OPM_PIN_CS)
#define OPM_MASK_WR   ((uint32_t)1u << OPM_PIN_WR)
#define OPM_MASK_RD   ((uint32_t)1u << OPM_PIN_RD)
#define OPM_MASK_IC   ((uint32_t)1u << OPM_PIN_IC)
#define OPM_MASK_IRQ  ((uint32_t)1u << OPM_PIN_IRQ)

#define OPM_MASK_ALL (OPM_MASK_DATA | OPM_MASK_A0 | OPM_MASK_CS | OPM_MASK_WR | OPM_MASK_RD | OPM_MASK_IC)

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

/* OPM_BUSY_CYCLES（φM サイクル）相当のシステムクロックサイクル数と、その ns 表示 */
static uint32_t s_data_wait_cycles;
static uint32_t s_data_wait_ns;

/*
 * φM の分周比と、それに付随する sys_clk 依存の値を算出する。
 *
 * φM = sys_clk / (2 x div) なので div = sys_clk / (2 x φM)。
 * これを 1/256 刻みの固定小数にすると div256 = sys_clk x 128 / φM。
 *
 * round_up を立てると 1/256 刻みではなく整数へ切り上げる。分周比が大きくなる方向
 * なので φM は目標以下にしかならず、H/L 期間が公称値より短くならない。クロック
 * 切り替えの途中（sys_clk が中途半端な 48MHz のとき）に使う。
 */
static void opm_clock_compute(uint32_t sys_hz, uint32_t phim_hz, bool round_up)
{
    uint64_t div256;

    if (round_up)
    {
        uint64_t den = 2ull * phim_hz;
        uint64_t div = ((uint64_t)sys_hz + den - 1u) / den;
        div256 = div << 8;
    }
    else
    {
        div256 = ((uint64_t)sys_hz * 128u + phim_hz / 2u) / phim_hz;
    }
    if (div256 < 256u)
    {
        div256 = 256u; /* 分周比 1 未満にはできない */
    }

    s_div_int = (uint32_t)(div256 >> 8);
    s_div_frac = (uint32_t)(div256 & 0xffu);
    s_clock_hz_actual = (uint32_t)(((uint64_t)sys_hz * 128u) / div256);

    /* ns 指定のセットアップ時間をサイクル数へ（切り上げ） */
    s_setup_cycles =
        (uint32_t)(((uint64_t)sys_hz * OPM_T_SETUP_NS + 999999999u) / 1000000000u);

    /*
     * データサイクル後の BUSY 待ち。φM サイクル数で持っているので、
     * sys_clk と φM の比を掛ければ sys クロックのサイクル数になる（切り上げ）。
     *
     * **目標値 phim_hz ではなく、上で求めた実 φM を使う。** round_up の経路では
     * 実 φM が目標以下になるので、目標で割ると BUSY が明ける前に待ちが終わる。
     * 実 φM で割っておけば、どの経路から呼ばれても待ちが足りなくなることはない。
     */
    s_data_wait_cycles = (uint32_t)(((uint64_t)sys_hz * OPM_BUSY_CYCLES +
                                     s_clock_hz_actual - 1u) /
                                    s_clock_hz_actual);
    s_data_wait_ns = (uint32_t)(((uint64_t)OPM_BUSY_CYCLES * 1000000000u +
                                 s_clock_hz_actual - 1u) /
                                s_clock_hz_actual);
}

/*
 * 算出済みの分周比を、走っている SM へ反映する。
 *
 * 走ったまま SMx_CLKDIV を書くと進行中のカウントの扱いが規定されていないので、
 * 一瞬止めてから書く。停止中は SM が最後に出したレベルをピンが保持するため、
 * 進行中の H/L 期間は数百 ns 伸びるだけで、短くはならない。
 * clkdiv_restart は分周カウンタを 0 に戻すだけで PC / X / Y には触らない。
 */
static void opm_clock_apply(void)
{
    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_sm_set_clkdiv_int_frac8(s_pio, s_sm, s_div_int, (uint8_t)s_div_frac);
    pio_sm_clkdiv_restart(s_pio, s_sm);
    pio_sm_set_enabled(s_pio, s_sm, true);
}

/* φM の分周比を算出して PIO を起動する。 */
static void opm_clock_start(void)
{
    opm_clock_compute(clock_get_hz(clk_sys), OPM_CLOCK_HZ, false);

    bool ok = pio_claim_free_sm_and_add_program(&opm_clock_program, &s_pio, &s_sm, &s_offset);
    hard_assert(ok);

    opm_clock_program_init(s_pio, s_sm, s_offset, OPM_PIN_PHIM, s_div_int, (uint8_t)s_div_frac);
    pio_sm_set_enabled(s_pio, s_sm, true);
}

void opm_clock_retune_for(uint32_t sys_hz, uint32_t phim_hz)
{
    opm_clock_compute(sys_hz, phim_hz, false);
    opm_clock_apply();
}

void opm_clock_retune_up(uint32_t phim_hz)
{
    opm_clock_compute(clock_get_hz(clk_sys), phim_hz, true);
    opm_clock_apply();
}

/*
 * データバスの向きを切り替える。
 *
 * 既定は出力（書き込み方向）で、読み出しの間だけ入力へ倒す。OPM に D0-D7 を
 * 駆動させる前に必ず入力にしておくこと（バスの衝突を防ぐ）。
 */
static void opm_bus_set_dir(bool out)
{
    if (out)
    {
        gpio_set_dir_out_masked(OPM_MASK_DATA);
    }
    else
    {
        gpio_set_dir_in_masked(OPM_MASK_DATA);
    }
}

/* データバスと A0 を確定させてから /CS と /WR を制御する（1 バスサイクル） */
static void opm_bus_cycle(bool a0, uint8_t value)
{
    /* データ・A0 と同時に /CS を L にする */
    gpio_put_masked(OPM_MASK_DATA | OPM_MASK_A0 | OPM_MASK_CS,
                    ((uint32_t)value << OPM_PIN_D0) | (a0 ? OPM_MASK_A0 : 0u));
    busy_wait_at_least_cycles(s_setup_cycles); /* t_SETUP */

    gpio_clr_mask(OPM_MASK_WR);
    busy_wait_us_32(OPM_T_WR_US);
    gpio_set_mask(OPM_MASK_WR);

    /* /WR を H に戻した後に /CS を H に戻す */
    gpio_set_mask(OPM_MASK_CS);
}

void opm_init(void)
{
    /* 出力ピンをまとめて初期化。/CS・/WR・/RD・/IC は負論理なので H から始める。 */
    gpio_init_mask(OPM_MASK_ALL);
    gpio_put_masked(OPM_MASK_ALL, OPM_MASK_CS | OPM_MASK_WR | OPM_MASK_RD | OPM_MASK_IC);
    gpio_set_dir_out_masked(OPM_MASK_ALL & ~OPM_MASK_DATA);
    opm_bus_set_dir(true); /* データバスは書き込み方向で始める */

    /* /IRQ を入力として初期化し、プルアップを有効にする */
    gpio_init(OPM_PIN_IRQ);
    gpio_set_dir(OPM_PIN_IRQ, GPIO_IN);
    gpio_pull_up(OPM_PIN_IRQ);

    /* /IC を操作する前に φM を動かしておく必要がある。s_setup_cycles もここで決まる。 */
    opm_clock_start();

    opm_reset();
}

void opm_write(uint8_t addr, uint8_t data)
{
#if STATS_PROFILE
    uint32_t t0 = time_us_32();
#endif

    /* アドレスサイクル */
    opm_bus_cycle(false, addr);
    busy_wait_us_32(OPM_T_ADDR_US);

    /*
     * データサイクル。BUSY は opm_read() で読めるが、書き込み経路では見ずに待つ。
     * 待つ長さは φM サイクル数から実行時に算出してあるので、クロックを
     * 切り替えても自動で追従する（opm.h の OPM_BUSY_CYCLES）。
     */
    opm_bus_cycle(true, data);
    busy_wait_at_least_cycles(s_data_wait_cycles);

#if STATS_PROFILE
    /*
     * 1 レジスタ書き込みの実費用。タイミング定数を変えたときの前後比較は
     * `s` の OPMW 行の平均（us/s ÷ writes/s）で見る。
     */
    stats_opm_write_add(time_us_32() - t0);
#endif
}

/*
 * /RD による 1 バイト読み出し。A0=1 がデータシート上のステータスレジスタ。
 *
 * 手順の並びがバス衝突の防止そのものなので崩さないこと。Pico 側を先に入力へ
 * 倒してから OPM に駆動させ、OPM が手放すのを待ってから出力へ戻す。
 */
uint8_t opm_read(bool a0)
{
    opm_bus_set_dir(false);

    /* A0 を確定させると同時に /CS を L にする（D0-D7 は入力中なので触らない） */
    gpio_put_masked(OPM_MASK_A0 | OPM_MASK_CS, a0 ? OPM_MASK_A0 : 0u);
    busy_wait_at_least_cycles(s_setup_cycles); /* t_SETUP */

    gpio_clr_mask(OPM_MASK_RD);
    busy_wait_us_32(OPM_T_RD_US);
    uint32_t all = gpio_get_all();
    gpio_set_mask(OPM_MASK_RD);

    /* /RD を H に戻した後に /CS を H に戻す */
    gpio_set_mask(OPM_MASK_CS);

    /* OPM が D0-D7 を手放すのを待ってから出力へ戻す */
    busy_wait_us_32(OPM_T_FLOAT_US);
    opm_bus_set_dir(true);

    return (uint8_t)((all >> OPM_PIN_D0) & 0xffu);
}

void opm_reset(void)
{
    gpio_clr_mask(OPM_MASK_IC);
    sleep_ms(OPM_T_IC_LOW_MS);
    gpio_set_mask(OPM_MASK_IC);
    sleep_ms(OPM_T_IC_WAIT_MS);
}

void opm_key_off_all(void)
{
    /*
     * 全 8 チャンネルをキーオフする。キーオフだけでは音色の RR 次第で音が長く
     * 残るので、先に全 32 スロットの RR を 15（最速）にしてから落とす。
     * (32 + 8) 回 x 32us = 約 1.3ms。
     *
     * **RR を全部書いてから、キーオフをまとめて撃つ。** ch ごとに交互に書くと最初と
     * 最後のチャンネルでキーオフが 1.3ms ずれ、リリースの残りも同じだけ滲む。
     * この順なら 8 回ぶんの 0.26ms に収まり、残りが明確な幅の窓になる。RR は
     * リリース位相でしか参照されないので、鳴っている最中に書いても何も起きない。
     *
     * opm_reset() は 20ms ブロックして I2S アンダーランが確定するので使わない。
     *
     * これは MXDRV 参照実装の L00063e が OPM に対して行うことと同じ。
     */

    /*
     * 1. 全 32 スロットの D1L/RR を D1L=0 / RR=15（最速リリース）に。
     *    KEY OFF より前に書くので、直前の音色の RR に関わらず即座に減衰する。
     *    0xff が uint8_t の上限なので、ループ変数は uint32_t で回す。
     */
    for (uint32_t reg = 0xe0; reg <= 0xff; reg++)
    {
        opm_write((uint8_t)reg, 0x0f);
    }

    /* 2. 全 8 チャンネルを KEY OFF */
    for (uint8_t ch = 0; ch < 8; ch++)
    {
        opm_write(0x08, ch);
    }
}

void opm_clear(void)
{
    /* 1-2. 最速リリースにしてから全チャンネルをキーオフ */
    opm_key_off_all();

    /* 3. 全 32 スロットの TL を最小音量に */
    for (uint8_t reg = 0x60; reg <= 0x7f; reg++)
    {
        opm_write(reg, 0x7f);
    }

    opm_write(0x0f, 0x00); /* 4. ノイズ off */
    opm_write(0x14, 0x00); /* 5. タイマ / IRQ 停止 */
    opm_write(0x01, 0x00); /* 6. LFO リセット解除 */
    opm_write(0x18, 0x00); /* 7. LFO 周波数 */

    /* 8. 0x19 は bit7 で書き込み先が切り替わる。PMD(0x80) と AMD(0x00) の両方を消す。 */
    opm_write(0x19, 0x80);
    opm_write(0x19, 0x00);

    opm_write(0x1b, 0x00); /* 9. LFO 波形 / CT1・CT2 */

    /* 10. RL / FB / CONNECT */
    for (uint8_t reg = 0x20; reg <= 0x27; reg++)
    {
        opm_write(reg, 0x00);
    }
}

uint32_t opm_clock_hz_actual(void)
{
    return s_clock_hz_actual;
}

uint32_t opm_clock_div_int(void)
{
    return s_div_int;
}

uint32_t opm_clock_div_frac(void)
{
    return s_div_frac;
}

uint32_t opm_data_wait_ns(void)
{
    return s_data_wait_ns;
}

bool opm_irq_level(void)
{
    return gpio_get(OPM_PIN_IRQ);
}
