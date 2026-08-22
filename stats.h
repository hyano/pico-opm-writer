/*
 * 実行時リソース使用量の計測
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef STATS_H
#define STATS_H

#include <stdbool.h>
#include <stdint.h>

/* ---- CPU 使用率 -------------------------------------------------------- */

/*
 * メインループの 1 周のうち、各サービス関数の中に居た時間を積む。
 *
 * サービス関数は連続して呼ばれるので、区間は 1 本で足りる。「何かが仕事をした
 * 周回はまるごと busy」という数え方はしない。それだと空回りの周回に含まれる
 * 固定費（USB スタックの処理など）まで仕事をした周回へ付け替わって、実負荷と
 * かけ離れた値になる（仕事をまとめて行うほど数字が下がる、といった逆転が起きる）。
 * 何もする事が無い周回はサービス関数がすぐ返るので、自然に 0 に近づく。
 */
void stats_busy_add(uint32_t busy_us);

/*
 * tud_task() の中に居た時間。毎周回走る固定費で、どのサービスの負荷でもないので
 * CPU 使用率とは別に持つ。
 */
void stats_usb_busy_add(uint32_t busy_us);

/* メインループから毎周回呼ぶ。1 秒窓が閉じたら CPU 使用率を確定する。 */
void stats_service(void);

uint32_t stats_cpu_percent(void);     /* 直近 1 秒窓の CPU 使用率 [%] */
uint32_t stats_cpu_percent_max(void); /* リセット以降の最大値 [%] */
uint32_t stats_usb_percent(void);     /* 直近 1 秒窓の tud_task() の占有率 [%] */

/* ---- DMA リング -------------------------------------------------------- */

/* 未処理フレーム数を通知する。high-water を更新する。 */
void stats_ring_update(uint32_t unread_frames);

uint32_t stats_ring_frames(void);     /* 現在の未処理フレーム数 */
uint32_t stats_ring_frames_max(void); /* リセット以降の最大値 */

/* ---- USB TX ------------------------------------------------------------ */

/* CDC #1 の TX FIFO 滞留バイト数を通知する。high-water を更新する。 */
void stats_usb_tx_update(uint32_t pending_bytes);

uint32_t stats_usb_tx_bytes(void);     /* 現在の滞留バイト数 */
uint32_t stats_usb_tx_bytes_max(void); /* リセット以降の最大値 */

/* ---- I2S 出力 ---------------------------------------------------------- */

/*
 * DMA の先を走っているフレーム数を通知する。low-water を更新する。
 * 余裕がどこまで削れたかを見たいので、ここだけは最大ではなく最小を残す。
 */
void stats_i2s_update(uint32_t depth_frames);

uint32_t stats_i2s_depth(void);     /* 現在の先行フレーム数 */
uint32_t stats_i2s_depth_min(void); /* リセット以降の最小値 */

/* ---- カウンタ ---------------------------------------------------------- */

void stats_count_overrun(void);            /* DMA overrun 発生 */
void stats_count_forbidden(uint32_t n);    /* YM3012 の禁止コード E=0 を n 個検出 */
void stats_count_rxstall(void);            /* PIO RX FIFO あふれ */
void stats_count_frames(uint32_t n);       /* 取り込んだフレーム数を n 加算 */
void stats_count_i2s_underrun(void);       /* I2S の先行分が尽きた */
void stats_count_pcm_clip(uint32_t n);     /* ADPCM を混ぜた結果 n サンプルが飽和した */

uint32_t stats_overrun(void);
uint64_t stats_forbidden(void);
uint32_t stats_rxstall(void);
uint64_t stats_frames(void);
uint32_t stats_i2s_underrun(void);
uint64_t stats_pcm_clip(void);

/* 直近 1 秒窓で数えた実測フレームレート [frames/s]。 */
uint32_t stats_frame_rate(void);

/* メインループの周回数。1 周あたりの固定費を見積もるのに使う。 */
uint32_t stats_loop_rate(void);

/* ---- フラッシュ書き込み ------------------------------------------------ */

/*
 * 4KiB ブロックを 1 回書き出した。消去と書き込みのあいだ IRQ は落ちていて
 * メインループが回らないので、その停止時間 (blackout) の最大値も残す。
 */
void stats_count_flash_write(void);
void stats_flash_blackout_add(uint32_t us);

uint32_t stats_flash_write(void);
uint32_t stats_flash_blackout_max_us(void);

/* ---- シーケンサ再生 (VGM / MDX で共用) ---------------------------------- */

/* スケジューラが予定時刻からどれだけ遅れたか。最大値を残す。 */
void stats_seq_lag_update(uint32_t us);
uint32_t stats_seq_lag_max_us(void);

/* ---- 初期化・リセット -------------------------------------------------- */

void stats_init(void);  /* 起動時に 1 回。全項目を 0 にする */
void stats_reset(void); /* 統計値のリセット（high-water とカウンタを 0 に戻す） */

#endif /* STATS_H */
