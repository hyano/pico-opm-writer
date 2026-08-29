/*
 * OPM (YM2151) バス制御
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPM_H
#define OPM_H

#include <stdint.h>
#include "pico/stdlib.h"

/* ---- クロック ---------------------------------------------------------- */

/*
 * φM のプリセット。φM と sys_clk は「整数分周（ジッタなし）になる組」でペアにして
 * あるので、片方だけを書き換えないこと。
 *
 *   _4MHZ : φM 4.000000MHz x 36 = sys 144MHz   → clkdiv 18。RP2350 定格 150MHz 以内
 *   _NTSC : φM 3.579545MHz x 44 = sys 157.5MHz → clkdiv 22。定格比 約 +5% の OC
 *
 * 実行時の切り替えは clockmode.h が持つ。ここの定数が決めるのは起動時の既定だけ。
 */
#define OPM_CLOCK_MODE_4MHZ 0
#define OPM_CLOCK_MODE_NTSC 1

/* 起動時のプリセット。この行の値、または cmake -DOPM_CLOCK_MODE=1 で決まる。 */
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

#define OPM_PIN_D0   2  /* D0-D7 = GP2-GP9。マスク書き込みのため連続であること */
#define OPM_PIN_A0   10 /* L=アドレスラッチ / H=データ書き込み */
#define OPM_PIN_CS   11 /* チップセレクト（負論理） */
#define OPM_PIN_WR   12 /* 書き込みストローブ（負論理） */
#define OPM_PIN_RD   13 /* 読み出しストローブ（負論理）。読み出し中だけ L */
#define OPM_PIN_IC   14 /* ハードウェアリセット（負論理） */
#define OPM_PIN_PHIM 15 /* マスタークロック（PIO 出力） */
#define OPM_PIN_IRQ  16 /* 割り込み要求（負論理）。入力・レベル参照のみ */

/* ---- タイミング定数 ---------------------------------------------------- */

#define OPM_T_SETUP_NS   100 /* データ確定から /WR 立ち下がりまで */
#define OPM_T_WR_US      1   /* /WR の L 期間 */
#define OPM_T_ADDR_US    5   /* アドレスラッチ後の待機 */
#define OPM_T_RD_US      1   /* /RD の L 期間（データが出揃うまでを含む） */
#define OPM_T_FLOAT_US   1   /* /RD を H に戻してからバスを出力へ戻すまで */
#define OPM_T_IC_LOW_MS  10  /* /IC の L 保持時間 */
#define OPM_T_IC_WAIT_MS 10  /* /IC を H に戻してから最初の書き込みまで */

/*
 * データサイクル後の BUSY 待ち。**µs ではなく φM サイクルで持つ。**
 *
 * BUSY は書き込みから **67.1 ± 0.2 φM サイクル**で落ちる。レジスタアドレスにも
 * 書き込む値にもチップの発音状態にも依存しない（[test/opm_busy/](../test/opm_busy/README.md)
 * の実測。データシート由来の 68 サイクルと 1 サイクル以内で一致する）。
 *
 * φM を単位にしておくと、待ち時間は `s_setup_cycles` と同じく実行時に
 * clk_sys から算出でき、φM プリセットを切り替えても定数の書き換えが要らない
 * （4MHz で 18.0µs / 3.579545MHz で 20.1µs）。
 *
 * 72 は実測の 67.1 に約 7% の余裕を足した値。67 と 68 のどちらなのかは
 * 測定で分離できておらず、BUSY 長の個体差・温度依存も未確認なため。
 * この値は test/opm_busy が実機で検証した 18µs（φM=4MHz）と同じ。
 * **BUSY ポーリングには置き換えない。** 効果は同じで副作用だけが増えることが
 * 実測で分かっている（同 README の「案 C を採る理由が無い」）。
 */
#define OPM_BUSY_CYCLES 72

/* ---- API --------------------------------------------------------------- */

void opm_init(void);                        /* GPIO と PIO(φM) を初期化し /IC リセットを実行 */
void opm_write(uint8_t addr, uint8_t data); /* 1 レジスタ書き込み（アドレス→データの 2 サイクル） */
uint8_t opm_read(bool a0);                  /* /RD による 1 バイト読み出し（A0=1 がステータスレジスタ） */
void opm_reset(void);                       /* /IC によるハードウェアリセット */
void opm_key_off_all(void);                 /* 最速リリースにしてから全 8ch をキーオフ */
void opm_clear(void);                       /* ソフトウェアによる全レジスタクリア */
bool opm_irq_level(void);                   /* /IRQ の現在のレベル（true = H = 非アサート） */

uint32_t opm_clock_hz_actual(void); /* 実際に生成されている φM 周波数 */
uint32_t opm_clock_div_int(void);   /* PIO 分周比の整数部 */
uint32_t opm_clock_div_frac(void);  /* PIO 分周比の小数部（/256） */
uint32_t opm_data_wait_ns(void);    /* 現在の φM での t_DATA（`i` の timing 行が出す） */

/*
 * 走ったまま φM の分周比を張り替える（clockmode.c が使う）。
 *
 * _for : 指定した sys_clk を前提に、φM がちょうど phim_hz になる分周比を入れる。
 *        clk_sys を上げる「前」に、上げたあとの値で呼ぶこと。
 * _up  : 現在の sys_clk を前提に、分周比を整数へ切り上げて入れる。φM は目標以下に
 *        しかならないので、H/L 期間が公称値より短くならない。
 *
 * どちらも SM を一瞬止めてから書くので、進行中の H/L 期間は伸びるだけで短くならない。
 */
void opm_clock_retune_for(uint32_t sys_hz, uint32_t phim_hz);
void opm_clock_retune_up(uint32_t phim_hz);

#endif /* OPM_H */
