/*
 * φM プリセットの実行時切り替えの実装
 *
 * SPDX-License-Identifier: MIT
 */
#include "clockmode.h"

#include <stdio.h>

#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"

#include "capture.h"
#include "i2s.h"
#include "opm.h"

/* プリセットの表。並びは clock_preset_t と同じ。 */
static const struct {
    const char *name;
    uint32_t phim_hz;
    uint32_t sys_khz;
} s_preset[CLOCK_PRESET_COUNT] = {
    [CLOCK_PRESET_4MHZ] = {"4", 4000000u, 144000u},
    [CLOCK_PRESET_NTSC] = {"3.58", 3579545u, 157500u},
};

#if OPM_CLOCK_MODE == OPM_CLOCK_MODE_4MHZ
#define CLOCK_PRESET_BOOT CLOCK_PRESET_4MHZ
#else
#define CLOCK_PRESET_BOOT CLOCK_PRESET_NTSC
#endif

static clock_preset_t s_preset_cur = CLOCK_PRESET_BOOT;
static bool s_auto = true;

/* ---- 初期化 ------------------------------------------------------------ */

void clockmode_init(void) {
    s_preset_cur = CLOCK_PRESET_BOOT;
    s_auto = true;
}

/* ---- プリセットの情報 -------------------------------------------------- */

clock_preset_t clockmode_preset(void) {
    return s_preset_cur;
}

const char *clockmode_preset_name(clock_preset_t p) {
    return s_preset[p].name;
}

uint32_t clockmode_preset_hz(clock_preset_t p) {
    return s_preset[p].phim_hz;
}

uint32_t clockmode_preset_sys_khz(clock_preset_t p) {
    return s_preset[p].sys_khz;
}

clock_preset_t clockmode_nearest(uint32_t hz) {
    /* 2 つしかないので中点で分ける。3789772Hz 未満なら NTSC 側。 */
    uint32_t mid = (s_preset[CLOCK_PRESET_NTSC].phim_hz + s_preset[CLOCK_PRESET_4MHZ].phim_hz) / 2u;
    return (hz < mid) ? CLOCK_PRESET_NTSC : CLOCK_PRESET_4MHZ;
}

/* ---- VGM への追従 ------------------------------------------------------ */

bool clockmode_auto(void) {
    return s_auto;
}

void clockmode_set_auto(bool enabled) {
    s_auto = enabled;
}

/* ---- 切り替え ---------------------------------------------------------- */

/*
 * sys_clk と PIO の分周比を張り替える。
 *
 * φM と BCK の H/L 期間 = 分周比の個数ぶんの clk_sys サイクルなので、短いパルスは
 * 「分周比が小さいまま clk_sys が上がる」ときにだけ出る。clk_sys 自体は glitchless
 * mux なので欠けたサイクルは出ず、上がる瞬間に進行中だった H/L 期間は最悪でも
 * 「新しい clk_sys での公称 H/L 期間」までしか縮まない。
 *
 * したがって守るべき規則は 1 つ。
 *
 *   clk_sys を上げる操作の直前には、その clk_sys に対応する最終の分周比が
 *   入っていること。分周比を書き換えるのは clk_sys を下げたあとにすること。
 *
 * 手順:
 *
 *   A. clk_sys を pll_usb (48MHz) へ退避する。pll_sys を触るには使用をやめる
 *      必要がある。下げ方向なので H/L は伸びるだけ。
 *   B. 48MHz を前提に分周比を張り直す。整数へ切り上げるので φM は目標以下に
 *      しかならず、H/L 期間は公称値を下回らない。C のあいだ φM も BCK も
 *      ほぼ公称値のまま動き続けるので、OPM も DAC も通常の動作範囲に留まる。
 *   C. pll_sys を新しい周波数へ再ロックする（数十〜数百 µs）。
 *   D. 新しい sys_clk を前提にした最終の分周比を入れる。48MHz のままなので φM は
 *      一時的に 1.1〜1.3MHz へ落ちるが、E までは約 1µs。
 *   E. clk_sys を pll_sys へ戻す。上げ方向だが最終の分周比が入っているので、
 *      最悪の H/L 期間は新プリセットの公称値ちょうど。
 *
 * clock_configure_undivided() は内部で一度 clk_ref (XOSC 12MHz) を経由するので、
 * A と E それぞれに数 µs の「clk_sys 12MHz」の窓ができる。これも下げ方向なので
 * 短いパルスにはならない（E で戻るときは最終分周比が効く）。この窓のあいだだけ
 * ym3012 キャプチャの PIO は φ1 に対する余裕が減るが、毎フレーム SH1 で再同期
 * するので次のフレームで復帰する。
 *
 * clk_peri と clk_ref は起動時の設定（pll_usb / XOSC）のままで触らない。
 */
static void clockmode_apply(uint32_t vco, uint post_div1, uint post_div2, uint32_t phim_hz) {
    uint32_t sys_hz = vco / (post_div1 * post_div2);

    /* A */
    clock_configure_undivided(clk_sys,
                              CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
                              CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
                              USB_CLK_HZ);

    /* B */
    opm_clock_retune_up(phim_hz);
    i2s_retune();

    /* C */
    pll_init(pll_sys, PLL_SYS_REFDIV, vco, post_div1, post_div2);

    /* D */
    opm_clock_retune_for(sys_hz, phim_hz);
    i2s_retune();

    /* E */
    clock_configure_undivided(clk_sys,
                              CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
                              CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
                              sys_hz);
}

const char *clockmode_set(clock_preset_t p) {
    if (p == s_preset_cur) {
        return NULL;
    }

    /*
     * キャプチャ中に切り替えると、ホストへ流している PCM のサンプリングレートが
     * ストリームの途中で変わってしまい、出来上がる WAV の時間軸が黙って狂う。
     */
    if (capture_state() != CAPTURE_STATE_IDLE) {
        printf("# hint    : cannot switch phiM while capturing PCM; run p 0 first\n");
        return "wrong state";
    }

    uint vco, post_div1, post_div2;
    if (!check_sys_clock_khz(s_preset[p].sys_khz, &vco, &post_div1, &post_div2)) {
        return "unsupported";
    }

    /*
     * 途中で割り込みが入っても壊れはしないが、遅い clk_sys の窓が伸びるだけ損。
     * 全体で数百 µs しか止まらない（フラッシュ書き込みのブラックアウトより短い）。
     */
    uint32_t save = save_and_disable_interrupts();
    clockmode_apply(vco, post_div1, post_div2, s_preset[p].phim_hz);
    restore_interrupts(save);

    s_preset_cur = p;
    return NULL;
}

const char *clockmode_follow_file(uint32_t file_clock_hz) {
    if (!s_auto || file_clock_hz == 0u) {
        return NULL;
    }
    return clockmode_set(clockmode_nearest(file_clock_hz));
}
