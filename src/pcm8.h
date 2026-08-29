/*
 * PCM8 (X68000 の ADPCM 多重再生ドライバ) 相当のソフトデコードとミキシング
 *
 * MDX の ADPCM パート (最大 8ch) を PDX から読んでソフトウェアでデコードし、
 * YM3012 から取り込んだ FM の PCM に加算する。加算点は
 * ym3012_reader_read_pcm() 1 箇所なので、USB キャプチャ (CDC #1) と I2S 出力の
 * 両方に同じ内容が乗る。
 *
 * 解釈は PCM8 version 0.48 の技術資料と MXDRV 2.06+16 Rel.3+25 の呼び出し方に準拠する
 * （どちらもソースは同梱していない）。
 *
 * 出力レートとの関係:
 *   出力は φM/64、ADPCM は φM/1024, /768, /512, /384, /256 なので、
 *   1 ソースサンプルはちょうど 16 / 12 / 8 / 6 / 4 出力フレームになる。
 *   常に整数比なので補間は行わず、サンプルをそのまま繰り返す
 *   （= MSM6258 の出力そのもの）。
 *
 * 音量レベルは実機に忠実な固定値にする。デコード結果の 12bit を 4bit 左シフトして
 * 16bit フルスケールとし、FM のフルスケールと同じ重みで足す。調整のつまみは無い。
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef PCM8_H
#define PCM8_H

#include <stdbool.h>
#include <stdint.h>

#include "mdx.h"

/*
 * MDX の ADPCM パート専用なので、指定が無ければ MDX_ENABLED に従う。
 * MDX を落としたまま PCM8 だけ残すと、誰も呼ばないデコーダとリング
 * （約 27KB）が居座ることになる。
 */
#ifndef PCM8_ENABLED
#define PCM8_ENABLED MDX_ENABLED
#endif

/* PCM8 のチャンネル数 */
#define PCM8_CH_MAX 8u

/* PDX のエントリ表。1 バンク = 96 音 x 8 バイト (BE offset + BE length)。 */
#define PDX_BANK_NOTES  96u
#define PDX_ENTRY_BYTES 8u
#define PDX_BANK_BYTES  (PDX_BANK_NOTES * PDX_ENTRY_BYTES)

/* チャンネルごとの先読み量。15.6kHz の ADPCM でも 131ms 分になる。 */
#define PCM8_PREFETCH_BYTES 1024u

/* ---- 初期化とサービス -------------------------------------------------- */

/* ym3012_init() のあとに 1 回呼ぶ。ミキサフックの登録もここで行う。 */
void pcm8_init(void);

/*
 * メインループから毎周回、どの消費者よりも先に呼ぶ。
 * ym3012_write_total() までミックスリングを前進レンダリングする。
 * 戻り値は「実際に発音していたか」（CPU 使用率の busy 判定に使う）。
 *
 * **発音状態を変える関数は、変える前にミックスリングを実時刻まで描き終えている。**
 * 下の発音・停止・モード変更・PDX の開閉・有効切り替えがすべてそうなっていて、
 * これが FM と ADPCM の発音時刻を合わせている（[docs §10.7](../docs/pico-opm-writer.md#107-adpcm-pcm8-の再生)）。
 * 呼び出し間隔を空けても、この関数が束ねても、発音の時刻は動かない。
 */
bool pcm8_service(void);

/*
 * フラッシュ書き込みなどでメインループが長時間止まったあとに呼ぶ。
 * レンダリング位置を現在の書き込み位置へ張り直す。
 */
void pcm8_resync(void);

/* ---- PDX -------------------------------------------------------------- */

/* PDX を開く。成功なら NULL、失敗ならエラー理由の文字列。 */
const char *pcm8_open_pdx(const char *path);
void pcm8_close_pdx(void);
bool pcm8_pdx_ready(void);
const char *pcm8_pdx_path(void); /* 開いている PDX のパス。無ければ空文字列 */

/* ---- 発音（PCM8 のファンクションコール相当）--------------------------- */

/*
 * $000x 通常出力。PDX の (bank, note) を鳴らす。
 * mode / vol / pan は PCM8 と同じく負値で「以前の値を保持」。
 *   mode: 0-4 = ADPCM (3.9/5.2/7.8/10.4/15.6kHz) / 5 = 16bit PCM / 6 = 8bit PCM
 *   vol : 0-15。1step = 2dB、8 が原音
 *   pan : 0 = 停止 / 1 = 左 / 2 = 右 / 3 = 左右。定位は全チャネル共通。
 */
void pcm8_key_on(uint32_t ch, uint32_t bank, uint32_t note, int mode, int vol, int pan);

/* $007x 動作モード変更。発音中のチャンネルの音量/周波数/定位を差し替える。 */
void pcm8_set_mode(uint32_t ch, int vol, int mode, int pan);

/* $000x で d2 = 0（チャネル停止）。音量/周波数/定位は変えない。 */
void pcm8_stop(uint32_t ch);

void pcm8_end_all(void);   /* $0100 終了 */
void pcm8_abort_all(void); /* $0101 一時停止（即時打ち切り） */

/* ---- 発音（PCM8 モードでないときの IOCS 経路）------------------------- */

/*
 * MDX の 0xE8（PCM8 モード宣言）が出ていない曲は、参照実装では PCM8 ではなく
 * IOCS を叩く。IOCS には音量もバンクも無いので、同じ PDX でも鳴り方が違う。
 */

/*
 * IOCS _ADPCMOUT ($60) 相当。PCM8 のファンクションコールと違い、音量は
 * 原音量 (8) 固定、バンクは 0 固定、長さは PDX エントリの下位 16bit だけを見る。
 * 実機の MSM6258 は 1 個なのでチャンネルは ch0 固定。
 */
void pcm8_iocs_out(uint32_t note, int mode, int pan);

/*
 * IOCS _ADPCMMOD ($67) 相当。abort = true が d1=1（中止）で即時停止、
 * false が d1=0（終了）。チェインを使っていないので「終了」は何もしない。
 */
void pcm8_iocs_mod(bool abort);

/* ---- 問い合わせ -------------------------------------------------------- */

uint32_t pcm8_active_mask(void); /* 発音中チャンネルのビットマスク */
uint32_t pcm8_active_count(void);
uint32_t pcm8_pan(void);        /* 現在の定位（全チャネル共通） */
uint32_t pcm8_read_count(void); /* PDX を読んだ回数 */

/*
 * キーオンの内訳。`mdx play`（PDX を開いた時点）と `s 0` の両方で 0 に戻る。
 *
 * 「曲が ADPCM を鳴らそうとしたのか」「鳴らそうとして失敗したのか」を、聴かずに
 * 切り分けられるようにしておく。ADPCM が終盤にしか出てこない曲もあるので、
 * 音が聞こえないことだけでは判断できない。
 */
uint32_t pcm8_keyon_count(void); /* 発音を開始した回数 */
uint32_t pcm8_miss_count(void);  /* 波形が無い / 音量 0 で鳴らせなかった回数 */

/* keyon / miss / reads を 0 に戻す（`s 0` から呼ぶ。PDX は閉じない） */
void pcm8_reset_counters(void);

/* FM だけの音と聴き比べるための切り替え。音量の調整つまみではない。 */
void pcm8_set_enabled(bool on);
bool pcm8_enabled(void);

/* ---- 自己テスト -------------------------------------------------------- */

/*
 * MSM6258 の ADPCM デコーダとレート比・音量表を既知のベクタで確かめる。
 * 実機の音は要らない。成功なら true、detail には内訳が入る。
 */
bool pcm8_selftest(const char **detail);

#endif /* PCM8_H */
