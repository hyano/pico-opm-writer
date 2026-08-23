/*
 * VGM / MDX の自動連続再生
 *
 * /VGM と /MDX のファイルを 1 本のプレイリストに束ね、曲の終わりを見張って次の曲へ
 * 送る。手で `vgm play` を打ち続ける代わりに、置いた曲を放っておいて鳴らすためのもの。
 *
 * 曲を送る条件は 2 つ。
 *   - 再生系が停止した（曲の終端に達したか、途中で壊れていた）
 *   - ループ回数が指定値に達した -> フェードアウトしてから停止
 * どちらの場合も、次の曲を始める前に無音の間隔を空ける。
 *
 * ループを持つ曲は vgm.c / mdx.c だけでは止まらない（どちらもループ回数を数えるだけ）
 * ので、打ち切りの判断はこのモジュールが持つ。
 *
 * フェードアウトは ym3012_fade_start() の出力ゲインで作る。**I2S 出力と USB キャプチャ
 * にしか効かず、YM3012 のアナログ出力は最後まで鳴っている**（音が止まるのはフェードが
 * 終わったあとのキーオフ）。ゲインを 1.0 へ戻すのは停止（キーオフ）の後だが、チップの
 * リリースが消えるまでの猶予を置いてから戻す（AUTOPLAY_RELEASE_MS）。
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef AUTOPLAY_H
#define AUTOPLAY_H

#include <stdbool.h>
#include <stdint.h>

#ifndef AUTOPLAY_ENABLED
#define AUTOPLAY_ENABLED 1
#endif

/*
 * プレイリストの上限。/VGM と /MDX の合計。
 *
 * 名前は '\0' 区切りでプールへ詰める。8.3 なら 12 バイト、長い名前でも数十バイトなので、
 * 12KB あれば 512 件はまず入る。どちらかが先に尽きたら `# warn` を出して打ち切る。
 */
#define AUTOPLAY_MAX_ENTRIES 512u
#define AUTOPLAY_POOL_BYTES  12288u

/* 既定値。autoplay stop では消えず、電源投入時にここへ戻る。 */
#define AUTOPLAY_LOOP_DEFAULT    2u
#define AUTOPLAY_FADE_MS_DEFAULT 2000u
#define AUTOPLAY_GAP_MS_DEFAULT  2000u

/*
 * 出力ゲインを 1.0 へ戻すまでの猶予と、戻すのにかける長さ。コマンドでは変えない。
 *
 * キーオフしてもチップの音はすぐには消えない。key_off_all() は RR を 15 にしてから
 * 落とすが、それでも実測で最悪 5.5ms かかる（KC=0 / KS=0 でリリースが最も遅くなる
 * 条件）。猶予を置かずに戻すと、その区間が全音量で出る。
 *
 * 猶予はフェードが 0 に達したあとの無音を延ばすだけなのでコストが無く、余裕を見て
 * 3 倍取ってある。ランプは猶予の見積もりが外れて音が残っていたときの保険で、
 * 曲送りで境界を前へ引き戻したときに次の曲の頭が段差にならない役目も兼ねる。
 */
#define AUTOPLAY_RELEASE_MS      16u
#define AUTOPLAY_RELEASE_RAMP_MS 4u

/* loop / fade / gap の上限（`d <ms>` と同じ 10 進） */
#define AUTOPLAY_LOOP_MAX 99u
#define AUTOPLAY_MS_MAX   60000u

typedef enum
{
    AUTOPLAY_STOPPED = 0,
    AUTOPLAY_PLAYING, /* 曲を鳴らしている */
    AUTOPLAY_FADING,  /* フェード中。終わったら停止して間隔へ */
    AUTOPLAY_GAP,     /* 曲間の無音 */
} autoplay_state_t;

typedef enum
{
    AUTOPLAY_MODE_LIST = 0, /* 一覧の順 */
    AUTOPLAY_MODE_RANDOM,   /* シャッフル */
} autoplay_mode_t;

typedef enum
{
    AUTOPLAY_SOURCE_BOTH = 0,
    AUTOPLAY_SOURCE_VGM,
    AUTOPLAY_SOURCE_MDX,
} autoplay_source_t;

/* ---- 初期化とサービス -------------------------------------------------- */

void autoplay_init(void);

/*
 * メインループから呼ぶ。曲の終わり・ループ回数・フェードと間隔の期限を見て
 * 状態を進める。戻り値は実仕事をしたかどうか。
 *
 * vgm_service() / mdx_service() より後に呼ぶこと。同じ周回で更新された再生状態を
 * そのまま見られる。
 */
bool autoplay_service(void);

/* ---- 操作 -------------------------------------------------------------- */

/*
 * プレイリストを作り直して 1 曲目を鳴らす（開始時は間隔を空けない）。
 * 成功なら NULL、失敗ならエラー理由の文字列（vgm_play() と同じ流儀）。
 */
const char *autoplay_start(void);

/*
 * 自動再生を止める。鳴っている曲も止めて出力ゲインを戻す。冪等。
 *
 * 手で `vgm play` / `mdx play` / `*_stop` を打ったときにも呼ぶ。走っていたときだけ
 * 通知を出す。
 */
const char *autoplay_stop(void);

/* 次 / 前の曲へ即座に移る。フェードも間隔も挟まない。停止中なら "wrong state"。 */
const char *autoplay_skip(int dir);

/* ---- 設定 -------------------------------------------------------------- */

/* 実行中に変えてもよい。次の判定から効く。 */
void autoplay_set_mode(autoplay_mode_t mode);
void autoplay_set_source(autoplay_source_t source);
void autoplay_set_loop(uint32_t loops); /* 0 = 無限（自然終了でだけ曲を送る） */
void autoplay_set_fade_ms(uint32_t ms); /* 0 = フェードせず即停止 */
void autoplay_set_gap_ms(uint32_t ms);  /* 0 = 間隔なし */

autoplay_mode_t autoplay_mode(void);
autoplay_source_t autoplay_source(void);
uint32_t autoplay_loop(void);
uint32_t autoplay_fade_ms(void);
uint32_t autoplay_gap_ms(void);

/* ---- 問い合わせ -------------------------------------------------------- */

autoplay_state_t autoplay_state(void);
const char *autoplay_state_name(void);

/* 停止していない（PLAYING / FADING / GAP のいずれか）。曲間でも true。 */
bool autoplay_is_running(void);

const char *autoplay_mode_name(void);
const char *autoplay_source_name(void);

uint32_t autoplay_count(void);    /* プレイリストの件数 */
uint32_t autoplay_position(void); /* 1 始まりの現在位置。停止中は 0 */

const char *autoplay_current_name(void); /* 現在の曲名。無ければ空文字列 */
bool autoplay_current_is_vgm(void);

/* プレイリストを 1 行ずつ出力する。tick は 1 行ごと（vgm_list() と同じ理由）。 */
const char *autoplay_print_list(void (*tick)(void));

#endif /* AUTOPLAY_H */
