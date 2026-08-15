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
 * 16KB = 4096 フレーム = 62500Hz で 65.5ms 分になる。
 *
 * ym3012 側のリングと一周時間を揃えてあるので、ポーリング間隔の制約は
 * 「65.5ms より十分短い間隔で呼ぶ」のまま変わらない。
 */
#define I2S_RING_BITS   14u
#define I2S_RING_BYTES  (1u << I2S_RING_BITS)
#define I2S_RING_FRAMES (I2S_RING_BYTES / 4u)

/*
 * DMA の読み出し位置より先へ書いておく目標フレーム数。これが出力レイテンシになる。
 * 1024 / 62500Hz = 16.4ms。
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

/* ---- 情報 -------------------------------------------------------------- */

uint32_t i2s_depth(void);      /* DMA の先を走っているフレーム数 */
uint32_t i2s_clkdiv_int(void); /* PIO 分周比の整数部 */
uint32_t i2s_clkdiv_frac(void);/* PIO 分周比の小数部（/256） */
uint32_t i2s_rate_hz(void);    /* 実際に出ているサンプリングレート */
uint32_t i2s_bck_hz(void);     /* 実際に出ている BCK 周波数 */

#endif /* I2S_H */
