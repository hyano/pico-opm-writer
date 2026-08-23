/*
 * MDX (X68000 / MXDRV) ファイルの再生
 *
 * /MDX/ に置かれた MDX を FatFs から読み、MXDRV 相当のシーケンサとして解釈して
 * YM2151 のレジスタ書き込みを発行する。VGM と違い MDX はバイナリ MML なので、
 * 音色・音量・ピッチ・LFO をこちら側で解釈してはじめてレジスタ値になる。
 *
 * 解釈は X68k MXDRV music driver version 2.06+17 Rel.X5-S / for Win32 [MXDRVg] V2.00b の
 * 仕様に合わせてある。一部の機能だけ MXDRV 2.06+16 Rel.3+25 に従う
 * （README のライセンス節）。
 *
 * ADPCM (MSM6258 / PDX) パートは本機にハードウェアが無いので、PCM8 相当の
 * ソフトデコーダ (pcm8.c) で鳴らす。ヘッダが要求する PDX を /MDX/<名前>.PDX から
 * 開き、キーオン / キーオフ / 音量 / 定位を PCM8 のファンクションコールと同じ形で
 * 渡す。FM 側と混ざるのは ym3012_reader_read_pcm() の中なので、I2S 出力と
 * USB キャプチャの両方に乗る。発音の有無はチャンネルごとの出力バックエンドで
 * 切り替える（ADPCM チャンネルも同期コマンド 0xEF/0xEE のためシーケンサとしては
 * FM と同じように回す）。
 *
 * タイミングは time_us_64() のポーリングで作る。MDX のテンポは OPM の Timer-B
 * 由来なので φM に依存する（VGM の 44100Hz 固定とはここが違う）。周期は
 * 1024 * (256 - tempo) φM サイクルちょうどなので、φM から誤差なく µs へ直せる。
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef MDX_H
#define MDX_H

#include <stdbool.h>
#include <stdint.h>

#include "filelist.h"

#ifndef MDX_ENABLED
#define MDX_ENABLED 1
#endif

/* MDX ファイルを置くディレクトリ */
#define MDX_DIR "/MDX"

/* mdx list が並べられるファイル数の上限（vgm.h の VGM_LIST_MAX と揃える） */
#define MDX_LIST_MAX 256u

/*
 * ファイル全体を RAM に載せる上限。
 *
 * チャンネルごとに独立したポインタがリピートやループで前後するので、VGM の
 * ような前方一方向のストリームにはできない。MDX 本体は典型 2-20KB なので
 * 丸ごと載せてしまう方が単純で速い。PDX は数百 KB あるので RAM には載せず、
 * pcm8.c が FatFs からストリーミングで読む（この上限とは別扱い）。
 */
#define MDX_MAX_BYTES 65536u

/*
 * チャンネル数。9 = FM 8 + ADPCM 1、16 = FM 8 + ADPCM 8 (PCM8 拡張)。
 * 参照実装のワークも FM8ch+PCM1ch と PCM7ch に分かれている。
 */
#define MDX_CH_MAX 16u
#define MDX_FM_CH  8u
#define MDX_PCM_CH 8u

/*
 * mdx_service() 1 回で opm_write() に使ってよい時間。
 * VGM_BUDGET_US と同じ理由（vgm.h:28-35）。1 tick の途中でも予算で抜けて
 * 次の周回から続きを処理する。
 */
#define MDX_BUDGET_US 500u

/* この時間より遅れたら時計を張り直す（長い停止のあとに早送りしない） */
#define MDX_RESYNC_LAG_US 200000u

/* タイトル / PDX 名の保持長。タイトルは Shift_JIS のまま持つ。 */
#define MDX_TITLE_MAX 128u
#define MDX_PDX_MAX   32u

typedef enum
{
    MDX_STATE_STOPPED = 0,
    MDX_STATE_PLAYING,
    MDX_STATE_ERROR, /* データの途中で壊れていた */
} mdx_state_t;

/* ---- 初期化とサービス -------------------------------------------------- */

void mdx_init(void);

/*
 * メインループから毎周回呼ぶ。予定時刻に達した tick を MDX_BUDGET_US 分だけ
 * 進める。戻り値は実仕事をしたかどうか。
 */
bool mdx_service(void);

/* ---- 操作 -------------------------------------------------------------- */

/*
 * /MDX/<name> を開いて解析し、再生を始める。
 * 成功なら NULL、失敗ならエラー理由の文字列（vgm_play() と同じ流儀）。
 *
 * VGM でも MDX でも、何かが再生中なら止めてから開く。ファイルを読み込んだあとで
 * 失敗したときは前の曲へは戻らず、停止状態のままエラーを返す。
 */
const char *mdx_play(const char *name);

/* 再生を止めて全チャンネルをキーオフする。冪等（停止中でも NULL）。 */
const char *mdx_stop(void);

/*
 * /MDX/ の .mdx を名前の昇順（大小無視）で 1 行ずつ出力する。
 * tick は 1 行出すごとに呼ばれる（vgm_list() と同じ理由）。
 */
const char *mdx_list(void (*tick)(void));

/*
 * mdx_list() と同じ対象を、出力せずに buf へ集める（autoplay のプレイリスト用）。
 * buf は追記されるので、呼び出し側が filelist_buf_reset() の要否を決める。
 */
const char *mdx_collect(filelist_buf_t *buf);

/* ---- 問い合わせ -------------------------------------------------------- */

mdx_state_t mdx_state(void);
const char *mdx_state_name(void);
bool mdx_is_playing(void);

const char *mdx_current_name(void); /* 再生中のファイル名。無ければ空文字列 */
const char *mdx_title(void);        /* タイトル (Shift_JIS のまま) */
const char *mdx_pdx_name(void);     /* ヘッダが要求する PDX 名。無ければ空文字列 */

uint32_t mdx_channels(void);     /* 9 または 16 */
uint64_t mdx_tick_count(void);   /* 発行済みの clock 数 */
uint32_t mdx_loop_count(void);   /* 曲が何周したか（全チャンネルがループ点に到達した回数） */
uint32_t mdx_reslip_count(void); /* 時計を張り直した回数 */
uint32_t mdx_tempo(void);        /* 現在の Timer-B 値 */
uint32_t mdx_tick_us(void);      /* 現在の 1 clock の長さ [us] */

/* ---- 自己テスト -------------------------------------------------------- */

/*
 * 音程 -> KC/KF と Timer-B 値 -> 1 clock の長さを既知のベクタで確かめる。
 * 実機の音は要らない。成功なら true、detail には内訳が入る。
 */
bool mdx_selftest(const char **detail);

#endif /* MDX_H */
