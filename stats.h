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

/* メインループが「仕事をした」区間を挟んで呼ぶ。busy_us を積算する。 */
void stats_busy_add(uint32_t busy_us);

/* メインループから毎周回呼ぶ。1 秒窓が閉じたら CPU 使用率を確定する。 */
void stats_service(void);

uint32_t stats_cpu_percent(void);     /* 直近 1 秒窓の CPU 使用率 [%] */
uint32_t stats_cpu_percent_max(void); /* リセット以降の最大値 [%] */

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

uint32_t stats_overrun(void);
uint64_t stats_forbidden(void);
uint32_t stats_rxstall(void);
uint64_t stats_frames(void);
uint32_t stats_i2s_underrun(void);

/* 直近 1 秒窓で数えた実測フレームレート [frames/s]。 */
uint32_t stats_frame_rate(void);

/* ---- 初期化・リセット -------------------------------------------------- */

void stats_init(void);  /* 起動時に 1 回。全項目を 0 にする */
void stats_reset(void); /* 統計値のリセット（high-water とカウンタを 0 に戻す） */

#endif /* STATS_H */
