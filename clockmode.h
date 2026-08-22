/*
 * φM プリセットの実行時切り替え
 *
 * φM は PIO の分周でしか作れず、PIO のクロック源は clk_sys しかない。4MHz と
 * 3.579545MHz (= 315/88 MHz) の両方が整数分周になる clk_sys は 2520MHz が最小で
 * 実現できないため、φM を変えるには sys_clk ごと張り替えるしかない。
 *
 * 切り替えの過程で φM / BCK / LRCK の H・L 期間が公称値より短くならないことを
 * 保証する。詳細は clockmode.c の手順のコメント。
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef CLOCKMODE_H
#define CLOCKMODE_H

#include <stdbool.h>
#include <stdint.h>

/* φM と sys_clk のペア。値は opm.h の OPM_CLOCK_MODE_* と一致させてある。 */
typedef enum
{
    CLOCK_PRESET_4MHZ = 0, /* φM 4.000000MHz / sys 144MHz   / clkdiv 18 */
    CLOCK_PRESET_NTSC = 1, /* φM 3.579545MHz / sys 157.5MHz / clkdiv 22 */
} clock_preset_t;

#define CLOCK_PRESET_COUNT 2

/* ---- 初期化 ------------------------------------------------------------ */

/*
 * 起動時のプリセット（opm.h の OPM_CLOCK_MODE）を記録する。
 * clk_sys と PIO は main() / opm_init() / i2s_init() が既に構成済みなので、
 * ここではハードウェアに触らない。i2s_init() より後に呼ぶこと。
 */
void clockmode_init(void);

/* ---- プリセットの情報 -------------------------------------------------- */

clock_preset_t clockmode_preset(void);
const char *clockmode_preset_name(clock_preset_t p); /* "4" / "3.58" */
uint32_t clockmode_preset_hz(clock_preset_t p);      /* 4000000 / 3579545 */
uint32_t clockmode_preset_sys_khz(clock_preset_t p); /* 144000 / 157500 */

/* 与えたクロック [Hz] に最も近いプリセット。境界は 2 つの中点。 */
clock_preset_t clockmode_nearest(uint32_t hz);

/* ---- VGM への追従 ------------------------------------------------------ */

bool clockmode_auto(void);
void clockmode_set_auto(bool enabled);

/* ---- 切り替え ---------------------------------------------------------- */

/*
 * プリセットを切り替える。成功なら NULL、失敗ならエラー文字列。
 * 既に同じプリセットなら何もせず NULL を返す。
 * PCM キャプチャ中はサンプリングレートが途中で変わってしまうので拒否する。
 */
const char *clockmode_set(clock_preset_t p);

/*
 * VGM ヘッダが申告するクロックへ合わせる（最寄りのプリセットへ寄せる）。
 * file_clock_hz が 0（不明）か、追従が off なら何もせず NULL を返す。
 */
const char *clockmode_follow_file(uint32_t file_clock_hz);

#endif /* CLOCKMODE_H */
