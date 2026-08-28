/*
 * 曲の終わり方の実装
 *
 * SPDX-License-Identifier: MIT
 */
#include "songend.h"

#include <stdio.h>

#include "pico/stdlib.h"

#include "mdx.h"
#include "opm.h"
#include "vgm.h"
#include "ym3012.h"

/* ---- 設定（電源投入時に既定へ戻る）------------------------------------ */

static uint32_t s_cfg_loop[SONGEND_KIND_COUNT];
static uint32_t s_cfg_fade_ms[SONGEND_KIND_COUNT];

/* ---- 状態 -------------------------------------------------------------- */

static songend_state_t s_state;

/* 今のトラックに適用中の値。play のたびに設定から取り込む。 */
static uint32_t s_loops;
static uint32_t s_fade_ms;

static uint32_t s_seq;          /* 見張っているトラックの通し番号 */
static uint64_t s_fade_end_us;  /* FADING の期限 */
static uint64_t s_ringout_us;   /* RINGOUT へ入った時刻 */
static bool s_ringout_cut;      /* 上限に達して強制消音したか（1 回だけ） */
static bool s_busy;             /* service_all() の再入よけ */

/* ---- 種別ごとの薄い当て板 ---------------------------------------------- */

static songend_kind_t cur_kind(void)
{
    return vgm_is_playing() ? SONGEND_KIND_VGM : SONGEND_KIND_MDX;
}

static bool cur_is_playing(void)
{
    return vgm_is_playing() || mdx_is_playing();
}

static uint32_t cur_loop_count(void)
{
    return vgm_is_playing() ? vgm_loop_count() : mdx_loop_count();
}

/*
 * ミリ秒をフレーム数へ。レートは φM/64。i2s_rate_hz() ではなくこちらから引くのは、
 * I2S_ENABLED=0 の構成でも同じ値が要るため。
 */
static uint32_t ms_to_frames(uint32_t ms)
{
    uint32_t rate = opm_clock_hz_actual() / 64u;
    return (uint32_t)(((uint64_t)ms * (uint64_t)rate) / 1000ull);
}

/* ---- 遷移 -------------------------------------------------------------- */

/*
 * 新しいトラックの見張りを始める。
 *
 * 停止側が置いた猶予を、曲が始まる時点まで引き戻す。曲間を空けない曲送りでは
 * 猶予の大半が新しい曲の頭に被る。境界は前へしか動かないので、猶予が済んでいれば
 * これは何もしない。
 */
static void begin_track(uint32_t loops, uint32_t fade_ms)
{
    s_seq = songend_track_seq();
    s_loops = loops;
    s_fade_ms = fade_ms;
    s_fade_end_us = 0;
    s_ringout_cut = false;
    ym3012_fade_release(0u, ms_to_frames(SONGEND_RELEASE_RAMP_MS));
    s_state = SONGEND_PLAYING;
}

/*
 * 余韻待ちへ入る。
 *
 * **出力ゲインの解除はキーオフの後**でなければならない。ym3012_fade_release() が
 * 戻すのは「これから取り込むフレーム」以降なので、先に止めておけば戻る対象は
 * 消音済みの区間だけになる。ただし**キーオフの直後ではまだ黙っていない**ので、
 * SONGEND_RELEASE_MS だけ待ってからランプで戻す。
 *
 * 自然終了ではキーオフそのものが走らない（余韻を鳴らし切る）が、その場合は
 * フェードも掛かっていないので、この呼び出しは何も動かさない。
 */
static void enter_ringout(void)
{
    ym3012_fade_release(ms_to_frames(SONGEND_RELEASE_MS),
                        ms_to_frames(SONGEND_RELEASE_RAMP_MS));
    s_ringout_us = time_us_64();
    s_ringout_cut = false;
    s_state = SONGEND_RINGOUT;
}

/*
 * 鳴っている方を止める。
 *
 * mdx_state() で囲むのは、MDX_ENABLED=0 のスタブの mdx_stop() が hint を出して
 * "unsupported" を返すため。
 */
static void stop_players(void)
{
    if (vgm_state() != VGM_STATE_STOPPED)
    {
        vgm_stop();
    }
    if (mdx_state() != MDX_STATE_STOPPED)
    {
        mdx_stop();
    }
}

static void begin_fade(void)
{
    /*
     * 起点はいま取り込み終えたフレーム。消費者はここより後ろを読んでいるので、
     * フェードは取りこぼしなく全部に掛かる。
     */
    ym3012_fade_start(ym3012_write_total(), ms_to_frames(s_fade_ms));
    s_fade_end_us = time_us_64() + (uint64_t)s_fade_ms * 1000ull;
    s_state = SONGEND_FADING;

    printf("# song    : fade out (loop %u)\n", (unsigned)s_loops);
}

/* ---- 初期化とサービス -------------------------------------------------- */

void songend_init(void)
{
    for (uint32_t i = 0; i < (uint32_t)SONGEND_KIND_COUNT; i++)
    {
        s_cfg_loop[i] = SONGEND_LOOP_DEFAULT;
        s_cfg_fade_ms[i] = SONGEND_FADE_MS_DEFAULT;
    }
    s_state = SONGEND_IDLE;
    s_loops = SONGEND_LOOP_DEFAULT;
    s_fade_ms = SONGEND_FADE_MS_DEFAULT;
    s_seq = songend_track_seq();
    s_ringout_cut = false;
    s_busy = false;
}

bool songend_service(void)
{
    if (s_busy)
    {
        return false;
    }
    s_busy = true;

    bool did = false;

    /* 新しいトラックが始まった。どの状態からでもここへ戻る。 */
    if (songend_track_seq() != s_seq)
    {
        songend_kind_t kind = cur_kind();
        begin_track(s_cfg_loop[kind], s_cfg_fade_ms[kind]);
        s_busy = false;
        return true;
    }

    switch (s_state)
    {
    case SONGEND_PLAYING:
        if (!cur_is_playing())
        {
            /* 自然終了・手動 stop・エラー。どれも余韻を待つのは同じ。 */
            enter_ringout();
            did = true;
        }
        else if (s_loops != 0u && cur_loop_count() >= s_loops)
        {
            if (s_fade_ms == 0u)
            {
                stop_players();
                enter_ringout();
            }
            else
            {
                begin_fade();
            }
            did = true;
        }
        break;

    case SONGEND_FADING:
        /*
         * 期限は壁時計で見る。I2S の先行は 1024 フレーム（約 16ms）しかなく、
         * その時点で鳴らし残しているのはゲインがほぼ 0 の区間なので足りる。
         * 曲の方が先に終わったときもここで拾う。
         */
        if (!cur_is_playing() || time_us_64() >= s_fade_end_us)
        {
            stop_players();
            enter_ringout();
            printf("# song    : end of fadeout\n");
            did = true;
        }
        break;

    case SONGEND_RINGOUT:
        if (ym3012_write_total() - ym3012_last_loud_total() >=
            (uint64_t)ms_to_frames(SONGEND_RINGOUT_QUIET_MS))
        {
            s_state = SONGEND_IDLE;
            did = true;
        }
        else if (!s_ringout_cut &&
                 time_us_64() - s_ringout_us >= (uint64_t)SONGEND_RINGOUT_MAX_MS * 1000ull)
        {
            /* 減衰しない音色が残っている。消してからもう一度無音を待つ。 */
            opm_key_off_all();
            s_ringout_cut = true;
            printf("# warn    : ringout cut at %u ms\n", (unsigned)SONGEND_RINGOUT_MAX_MS);
            did = true;
        }
        break;

    case SONGEND_IDLE:
    default:
        break;
    }

    s_busy = false;
    return did;
}

/* ---- 観測 -------------------------------------------------------------- */

songend_state_t songend_state(void)
{
    return s_state;
}

const char *songend_state_name(void)
{
    switch (s_state)
    {
    case SONGEND_IDLE:
        return "IDLE";
    case SONGEND_PLAYING:
        return "PLAYING";
    case SONGEND_FADING:
        return "FADING";
    case SONGEND_RINGOUT:
        return "RINGOUT";
    default:
        return "?";
    }
}

bool songend_is_active(void)
{
    return s_state != SONGEND_IDLE;
}

uint32_t songend_track_seq(void)
{
    return vgm_play_seq() + mdx_play_seq();
}

/* ---- 操作 -------------------------------------------------------------- */

void songend_arm(uint32_t loops, uint32_t fade_ms)
{
    begin_track(loops, fade_ms);
}

void songend_stop_playback(void)
{
    stop_players();
    enter_ringout();
}

/* ---- 設定 -------------------------------------------------------------- */

void songend_set_loop(songend_kind_t kind, uint32_t loops)
{
    if (kind < SONGEND_KIND_COUNT)
    {
        s_cfg_loop[kind] = loops;
    }
}

uint32_t songend_loop(songend_kind_t kind)
{
    return (kind < SONGEND_KIND_COUNT) ? s_cfg_loop[kind] : 0u;
}

void songend_set_fade_ms(songend_kind_t kind, uint32_t ms)
{
    if (kind < SONGEND_KIND_COUNT)
    {
        s_cfg_fade_ms[kind] = ms;
    }
}

uint32_t songend_fade_ms(songend_kind_t kind)
{
    return (kind < SONGEND_KIND_COUNT) ? s_cfg_fade_ms[kind] : 0u;
}
