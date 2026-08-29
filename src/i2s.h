/*
 * キャプチャした YM3012 の PCM を I2S で外部 DAC (PCM5102A) へ出力する
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef I2S_H
#define I2S_H

#include <stdbool.h>
#include <stdint.h>

#ifndef I2S_ENABLED
#define I2S_ENABLED 1
#endif

/* ---- ピン割り当て ------------------------------------------------------ */

/* BCK と LRCK は PIO の sideset base から連続していること */
#define I2S_PIN_BCK  26
#define I2S_PIN_LRCK 27
#define I2S_PIN_DIN  28

/* ---- リング ------------------------------------------------------------ */

/*
 * DMA のハードウェアリング機能を使うのでサイズは 2 の冪。
 * 1 フレーム (L+R) がちょうど 1 エントリ (32bit) なので、
 * 16KB = 4096 フレームで φM 4MHz なら 62500Hz の 65.5ms 分になる。
 * φM を下げると一周時間は伸びる（3.579545MHz で 73.2ms）ので制約は緩む方向。
 *
 * ym3012 側のリングと一周時間を揃えてあるので、ポーリング間隔の制約は
 * 「65.5ms より十分短い間隔で呼ぶ」のまま変わらない。
 */
#define I2S_RING_BITS   14u
#define I2S_RING_BYTES  (1u << I2S_RING_BITS)
#define I2S_RING_FRAMES (I2S_RING_BYTES / 4u)

/*
 * DMA の読み出し位置より先へ書いておく目標フレーム数。これが出力レイテンシになる。
 * φM 4MHz なら 1024 / 62500Hz = 16.4ms。
 */
#define I2S_TARGET_FRAMES 1024u

/* ---- 初期化 ------------------------------------------------------------ */

/*
 * PIO と DMA を起動する。以後 I2S 出力は常時動作し、停止しない。
 * φM の分周比を使うので opm_init() より後に呼ぶこと。
 */
void i2s_init(void);

/*
 * DMA の読み出し位置を追いかけ、消費された分だけリングを埋める。
 * リングが一周する 65.5ms より十分短い間隔で呼ぶこと（メインループから毎周回）。
 * 戻り値は CPU が実仕事をしたかどうか。
 */
bool i2s_service(void);

/*
 * φM プリセットの切り替えに追従して PIO の分周比を張り替える（clockmode.c が使う）。
 * SM は再起動せず、一瞬止めて分周比だけを書く。BCK / LRCK の H/L 期間は伸びる
 * ことはあっても短くならない。
 */
void i2s_retune(void);

/*
 * 65.5ms を超えて止まったあと（フラッシュの消去・書き込みなど）に呼ぶ。
 * DMA の読み出し位置を読み直して基準点を張り直し、ソース側カーソルを同期し、
 * 先行分を無音で埋め直す。統計にはアンダーラン 1 回として数える。
 */
void i2s_resync(void);

/*
 * 出力の有効・無効。無効中はソース側リングを読まず無音だけを流す。
 * BCK / LRCK と DMA は止めない（止めると DAC がポップする）。
 * ストレージが HOST モードのあいだ、フラッシュ書き込みと衝突させないために使う。
 */
void i2s_set_enabled(bool enabled);
bool i2s_enabled(void);

/* ---- 情報 -------------------------------------------------------------- */

uint32_t i2s_depth(void);       /* DMA の先を走っているフレーム数 */
uint32_t i2s_clkdiv_int(void);  /* PIO 分周比の整数部 */
uint32_t i2s_clkdiv_frac(void); /* PIO 分周比の小数部（/256） */
uint32_t i2s_rate_hz(void);     /* 実際に出ているサンプリングレート */
uint32_t i2s_bck_hz(void);      /* 実際に出ている BCK 周波数 */

#endif /* I2S_H */
