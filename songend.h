/*
 * 曲の終わり方
 *
 * 「曲の一生」を 1 本の状態機械で持つ。ループ回数で打ち切るか、自然に終わるまで
 * 待つか、終わったあとの余韻をどこまで待つか — その仕様と処理はここにしかない。
 *
 *              play                 loop 上限          fade 期限
 *   IDLE ───────────> PLAYING ─────────────────> FADING ─────────┐
 *    ▲                  │                                        │
 *    │                  │ 自然終了 / stop / error                 │ *_stop()
 *    │                  ▼                                        ▼
 *    └───── 無音 ──── RINGOUT <───────────────────────────────────┘
 *                       │
 *                   play（曲送り / 次の曲）──> PLAYING
 *
 * 外に出す観測は実質 songend_is_active() の 1 つで、autoplay の曲送りも
 * `p 2` のキャプチャの終端もこれを見る。**両者が同じ時刻で動くのはこのため。**
 *
 * ループを持つ曲は vgm.c / mdx.c だけでは止まらない（どちらもループ回数を数える
 * だけ）ので、打ち切りの判断はこのモジュールが持つ。
 *
 * フェードアウトは ym3012_fade_start() の出力ゲインで作る。**I2S 出力と USB
 * キャプチャにしか効かず、YM3012 のアナログ出力は最後まで鳴っている**（音が
 * 止まるのはフェードが終わったあとのキーオフ）。
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef SONGEND_H
#define SONGEND_H

#include <stdbool.h>
#include <stdint.h>

/* 設定は VGM と MDX で別々に持つ（`vgm loop` / `mdx loop` に 1 対 1 で対応する）。 */
typedef enum
{
    SONGEND_KIND_VGM = 0,
    SONGEND_KIND_MDX,
    SONGEND_KIND_COUNT,
} songend_kind_t;

typedef enum
{
    SONGEND_IDLE = 0,  /* 鳴っていない */
    SONGEND_PLAYING,   /* 演奏中 */
    SONGEND_FADING,    /* ループ上限に達してフェード中。演奏はまだ続いている */
    SONGEND_RINGOUT,   /* 演奏は止まった。余韻が消えるのを待っている */
} songend_state_t;

/* loop / fade の上限（`autoplay loop` / `autoplay fade` と同じ範囲） */
#define SONGEND_LOOP_MAX    99u
#define SONGEND_FADE_MS_MAX 60000u

/*
 * 既定値。**ループ上限の既定は 0（無限）**で、手動再生の従来の挙動を変えない。
 * autoplay は自分の既定（AUTOPLAY_LOOP_DEFAULT）を songend_arm() で上書きする。
 */
#define SONGEND_LOOP_DEFAULT    0u
#define SONGEND_FADE_MS_DEFAULT 2000u

/*
 * 演奏が止まってから余韻が消えるのを待つ長さ。
 *
 * QUIET が 100ms なのは、**波形が 1 周期に 2 回ゼロを通る**ため。瞬時値だけを見ると
 * 鳴っていても「無音」に見えるので、可聴下限（20Hz = 50ms 周期）を跨ぐ長さが要る。
 *
 * MAX に達したら opm_key_off_all() で強制的に消音してから、もう一度 QUIET を待つ。
 * RR=0 の音色が残ったまま曲が終わると減衰しないので、待つのをやめるだけでは
 * チップが鳴りっぱなしになる。**参照実装は自然終了後の音を止めないので、ここは
 * 保険としての意図的な逸脱。**
 */
#define SONGEND_RINGOUT_QUIET_MS 100u
#define SONGEND_RINGOUT_MAX_MS   5000u

/*
 * 出力ゲインを 1.0 へ戻すまでの猶予と、戻すのにかける長さ。コマンドでは変えない。
 *
 * キーオフしてもチップの音はすぐには消えない。opm_key_off_all() は RR を 15 に
 * してから落とすが、それでも実測で最悪 5.5ms かかる（KC=0 / KS=0 でリリースが
 * 最も遅くなる条件）。猶予を置かずに戻すと、その区間が全音量で出る。
 *
 * 猶予はフェードが 0 に達したあとの無音を延ばすだけなのでコストが無く、余裕を見て
 * 3 倍取ってある。ランプは猶予の見積もりが外れて音が残っていたときの保険で、
 * 曲送りで境界を前へ引き戻したときに次の曲の頭が段差にならない役目も兼ねる。
 */
#define SONGEND_RELEASE_MS      16u
#define SONGEND_RELEASE_RAMP_MS 4u

/* ---- 初期化とサービス -------------------------------------------------- */

void songend_init(void);

/*
 * メインループから呼ぶ。**vgm_service() / mdx_service() の後、autoplay_service() の
 * 前**に置くこと。同じ周回で更新された再生状態を見て、その結果を autoplay が
 * そのまま曲送りに使える。戻り値は実仕事をしたかどうか。
 */
bool songend_service(void);

/* ---- 観測 -------------------------------------------------------------- */

songend_state_t songend_state(void);
const char *songend_state_name(void);

/*
 * 曲がまだ終わっていない（PLAYING / FADING / RINGOUT のいずれか）。
 * **余韻が鳴っている間も true。** autoplay の曲送りとキャプチャの終端はこれで決まる。
 */
bool songend_is_active(void);

/*
 * 再生を始めた回数の合計。値が変わったら新しいトラックが始まっている。
 * 鳴っている最中に別の曲を play しても再生状態は落ちないので、レベルでは拾えない。
 */
uint32_t songend_track_seq(void);

/* ---- 操作 -------------------------------------------------------------- */

/*
 * 今のトラックに適用する値を差し替えて、最初から見張り直す。
 * **`*_play()` が成功した直後に呼ぶ。** 呼ばなければ設定（songend_set_loop 等）の値が
 * そのまま使われるので、手で `vgm play` したときはこれを呼ぶ必要はない。
 */
void songend_arm(uint32_t loops, uint32_t fade_ms);

/* 鳴っている方を止めて余韻待ちへ移る。冪等。 */
void songend_stop_playback(void);

/* ---- 設定（`vgm loop` / `vgm fade` と MDX 版の保存先）------------------ */

void songend_set_loop(songend_kind_t kind, uint32_t loops); /* 0 = 無限 */
uint32_t songend_loop(songend_kind_t kind);
void songend_set_fade_ms(songend_kind_t kind, uint32_t ms); /* 0 = フェードせず即停止 */
uint32_t songend_fade_ms(songend_kind_t kind);

#endif /* SONGEND_H */
