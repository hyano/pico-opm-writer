/*
 * VGM ファイルの再生
 *
 * /VGM/ に置かれた VGM を FatFs から読みながら、YM2151 のレジスタ書き込みを
 * VGM のタイムスタンプどおりに発行する。演奏対象は YM2151 だけで、他の音源の
 * コマンドは長さぶん読み飛ばす。
 *
 * gzip 圧縮された .vgz も、一時ファイルを作らずストリームのまま展開して再生する
 * （vgz.h）。圧縮の有無はファイル先頭のマジックで判定するので、拡張子には依存
 * しない。
 *
 * タイミングは time_us_64() のポーリングで作る。VGM のサンプルクロックは
 * 常に 44100Hz。φM は sys_clk の整数分周で、どちらも同じ水晶から出ているので
 * 長期的なずれは生じない。
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef VGM_H
#define VGM_H

#include <stdbool.h>
#include <stdint.h>

#include "filelist.h"

/* VGM のサンプルクロック。仕様で固定。 */
#define VGM_SAMPLE_RATE 44100u

/*
 * vgm_service() 1 回で opm_write() に使ってよい時間。
 *
 * メインループは 1 周回で標準入力を 1 文字しか読まないので、ここを大きくすると
 * コマンド入力が遅くなる（1.5ms にすると 660 文字/秒まで落ちる）。500us なら
 * 約 2000 文字/秒。追いつき能力は opm_write() が 32us なので約 28k writes/s で、
 * VGM が要求する量に対して十分。
 */
#define VGM_BUDGET_US 500u

/* この時間より遅れたら時計を張り直す（長い停止のあとに早送りしない） */
#define VGM_RESYNC_LAG_US 200000u

/*
 * .vgz のとき 1 回のバッファ補充で展開するバイト数。
 *
 * 非圧縮なら 4096 バイトの f_read は XIP からの memcpy 1 回で終わるが、
 * 展開は 1 バイトあたり十数サイクルかかるので同じ量にすると 1ms を超えて
 * VGM_BUDGET_US を割る。1024 バイトなら約 250us に収まる。
 */
#define VGM_GZ_CHUNK 1024u

/* VGM ファイルを置くディレクトリ */
#define VGM_DIR "/VGM"

/* vgm list が並べられるファイル数の上限 */
#define VGM_LIST_MAX 256u

typedef enum
{
    VGM_STATE_STOPPED = 0,
    VGM_STATE_PLAYING,
    VGM_STATE_ERROR, /* ストリームの途中で壊れていた */
} vgm_state_t;

/* ---- 初期化とサービス -------------------------------------------------- */

void vgm_init(void);

/*
 * メインループから毎周回呼ぶ。予定時刻に達したコマンドを VGM_BUDGET_US 分だけ
 * 実行する。遅れた分は次の周回で詰める。戻り値は実仕事をしたかどうか。
 */
bool vgm_service(void);

/* ---- 操作 -------------------------------------------------------------- */

/*
 * /VGM/<name> を開いてヘッダを検証し、再生を始める。
 * 成功なら NULL、失敗ならエラー理由の文字列（capture_start() と同じ流儀）。
 * ファイル形式のエラーはここで同期的に判明する。
 *
 * VGM でも MDX でも、何かが再生中なら止めてから開く。ファイルを開いたあとで
 * 失敗したときは前の曲へは戻らず、停止状態のままエラーを返す。
 */
const char *vgm_play(const char *name);

/* 再生を止めて全チャンネルをキーオフする。冪等（停止中でも NULL）。 */
const char *vgm_stop(void);

/*
 * /VGM/ の .vgm と .vgz を名前の昇順（大小無視）で 1 行ずつ出力する。
 *
 * tick は 1 行出すごとに呼ばれる。PICO_STDIO_USB_STDOUT_TIMEOUT_US が 10ms
 * あるので、行数が多いと printf の合計待ち時間がリング一周 65.5ms を超えうる。
 * ここに service_all() を渡してキャプチャと I2S を止めない。
 *
 * 成功なら NULL、失敗ならエラー理由の文字列。
 */
const char *vgm_list(void (*tick)(void));

/*
 * vgm_list() と同じ対象を、出力せずに buf へ集める（autoplay のプレイリスト用）。
 * buf は追記されるので、呼び出し側が filelist_buf_reset() の要否を決める。
 */
const char *vgm_collect(filelist_buf_t *buf);

/* ---- 問い合わせ -------------------------------------------------------- */

vgm_state_t vgm_state(void);
const char *vgm_state_name(void);
bool vgm_is_playing(void);

const char *vgm_current_name(void);  /* 再生中のファイル名。無ければ空文字列 */
uint64_t vgm_position_samples(void); /* 発行済みのサンプル位置 */
uint32_t vgm_total_samples(void);    /* ヘッダの総サンプル数。不明なら 0 */
uint32_t vgm_loop_count(void);
uint32_t vgm_reslip_count(void); /* 時計を張り直した回数 */

bool vgm_is_compressed(void); /* 再生中のファイルが .vgz か */

/*
 * ループ先頭の展開器の状態を保存できず、先頭から展開し直した回数。
 * 0 でないと、ループのたびに音が数百 ms 途切れている。
 */
uint32_t vgm_gz_reload_count(void);

/* ヘッダが申告する YM2151 のクロック [Hz]。不明なら 0。 */
uint32_t vgm_file_clock_hz(void);

#endif /* VGM_H */
