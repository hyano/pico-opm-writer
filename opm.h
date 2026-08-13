/*
 * OPM (YM2151) バス制御
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPM_H
#define OPM_H

#include <stdint.h>
#include "pico/stdlib.h"

#define OPM_WRITER_VERSION "0.2.0"

/* ---- クロック ---------------------------------------------------------- */

/*
 * φM のプリセット。φM と sys_clk は「整数分周（ジッタなし）になる組」でペアにして
 * あるので、片方だけを書き換えないこと。
 *
 *   _4MHZ : φM 4.000000MHz x 36 = sys 144MHz   → clkdiv 18。RP2350 定格 150MHz 以内
 *   _NTSC : φM 3.579545MHz x 44 = sys 157.5MHz → clkdiv 22。定格比 約 +5% の OC
 */
#define OPM_CLOCK_MODE_4MHZ 0
#define OPM_CLOCK_MODE_NTSC 1

/* 切り替えはこの行の値、または cmake -DOPM_CLOCK_MODE=1 で行う。 */
#ifndef OPM_CLOCK_MODE
#define OPM_CLOCK_MODE OPM_CLOCK_MODE_4MHZ
#endif

#if OPM_CLOCK_MODE == OPM_CLOCK_MODE_4MHZ
#define OPM_CLOCK_HZ      4000000u /* YM2151 の定格上限 */
#define OPM_SYS_CLOCK_KHZ 144000u
#elif OPM_CLOCK_MODE == OPM_CLOCK_MODE_NTSC
#define OPM_CLOCK_HZ      3579545u /* NTSC カラーサブキャリア = 315/88 MHz */
#define OPM_SYS_CLOCK_KHZ 157500u
#else
#error "OPM_CLOCK_MODE の値が不正です"
#endif

/* ---- ピン割り当て ------------------------------------------------------ */

#define OPM_PIN_D0   2   /* D0-D7 = GP2-GP9。マスク書き込みのため連続であること */
#define OPM_PIN_A0   10  /* L=アドレスラッチ / H=データ書き込み */
#define OPM_PIN_CS   11  /* チップセレクト（負論理） */
#define OPM_PIN_WR   12  /* 書き込みストローブ（負論理） */
#define OPM_PIN_RD   13  /* 読み出しストローブ（負論理）。現在は H 固定 */
#define OPM_PIN_IC   14  /* ハードウェアリセット（負論理） */
#define OPM_PIN_PHIM 15  /* マスタークロック（PIO 出力） */
#define OPM_PIN_IRQ  16  /* 割り込み要求（負論理）。入力・レベル参照のみ */

/* ---- タイミング定数 ---------------------------------------------------- */

#define OPM_T_SETUP_NS   100 /* データ確定から /WR 立ち下がりまで */
#define OPM_T_WR_US      1   /* /WR の L 期間 */
#define OPM_T_ADDR_US    5   /* アドレスラッチ後の待機 */
#define OPM_T_DATA_US    25  /* データ書き込み後の BUSY 待ち */
#define OPM_T_IC_LOW_MS  10  /* /IC の L 保持時間 */
#define OPM_T_IC_WAIT_MS 10  /* /IC を H に戻してから最初の書き込みまで */

/* ---- API --------------------------------------------------------------- */

void opm_init(void);                        /* GPIO と PIO(φM) を初期化し /IC リセットを実行 */
void opm_write(uint8_t addr, uint8_t data); /* 1 レジスタ書き込み（アドレス→データの 2 サイクル） */
void opm_reset(void);                       /* /IC によるハードウェアリセット */
void opm_clear(void);                       /* ソフトウェアによる全レジスタクリア */
bool opm_irq_level(void);                   /* /IRQ の現在のレベル（true = H = 非アサート） */

uint32_t opm_clock_hz_actual(void); /* 実際に生成されている φM 周波数 */
uint32_t opm_clock_div_int(void);   /* PIO 分周比の整数部 */
uint32_t opm_clock_div_frac(void);  /* PIO 分周比の小数部（/256） */

#endif /* OPM_H */
