/*
 * VGM / MDX の自動連続再生の実装
 *
 * SPDX-License-Identifier: MIT
 */
#include "autoplay.h"

#include <stdio.h>
#include <string.h>

#include "pico/rand.h"
#include "pico/stdlib.h"

#include "filelist.h"
#include "mdx.h"
#include "opm.h"
#include "storage.h"
#include "vgm.h"
#include "ym3012.h"

#if AUTOPLAY_ENABLED

/* ---- 設定（autoplay stop では消えない）-------------------------------- */

static autoplay_mode_t s_mode;
static autoplay_source_t s_source;
static uint32_t s_loop;
static uint32_t s_fade_ms;
static uint32_t s_gap_ms;

/* ---- プレイリスト ------------------------------------------------------ */

/*
 * 名前は '\0' 区切りでプールへ詰め、s_offs にプールへのオフセットを名前の昇順で
 * 並べる。曲の種別は配列を持たず、s_offs 上の位置が s_vgm_count 未満かどうかで
 * 判別する（filelist_collect() が /VGM の分を先に、/MDX の分をその後ろに置く）。
 */
/* filelist_buf_t のオフセットは uint16_t。プールがこれを超えると詰められない。 */
_Static_assert(AUTOPLAY_POOL_BYTES <= 65536u, "プールが uint16_t のオフセットに収まらない");
_Static_assert(AUTOPLAY_MAX_ENTRIES <= 65535u, "件数が uint16_t の添字に収まらない");

static char s_pool[AUTOPLAY_POOL_BYTES];
static uint16_t s_offs[AUTOPLAY_MAX_ENTRIES];

/* 鳴らす順。list では恒等、random ではシャッフルした並び。値は s_offs の添字。 */
static uint16_t s_order[AUTOPLAY_MAX_ENTRIES];

static uint32_t s_count;     /* プレイリストの件数 */
static uint32_t s_vgm_count; /* s_offs の [0, s_vgm_count) が VGM。残りが MDX */
static uint32_t s_pos;       /* s_order 上の現在位置 */
static uint16_t s_prev_idx;  /* 直前に鳴らした s_offs の添字。0xffff = 無し */

/* ---- 状態 -------------------------------------------------------------- */

static autoplay_state_t s_state;
static uint64_t s_fade_end_us;
static uint64_t s_gap_end_us;

/*
 * autoplay_service() を抑止するフラグ。
 *
 * `vgm list` / `mdx list` / `autoplay list` は 1 行ごとに service_all() を tick として
 * 呼ぶので、一覧を出している最中にここから曲送りが走りうる。プレイリストを作る間と
 * 出す間は動かさない。
 */
static bool s_busy;

/* ---- プレイリストの参照 ------------------------------------------------ */

static bool idx_is_vgm(uint32_t idx) {
    return idx < s_vgm_count;
}

static const char *idx_name(uint32_t idx) {
    return &s_pool[s_offs[idx]];
}

static uint32_t cur_idx(void) {
    return s_order[s_pos];
}

static bool cur_is_playing(void) {
    return idx_is_vgm(cur_idx()) ? vgm_is_playing() : mdx_is_playing();
}

static uint32_t cur_loops(void) {
    return idx_is_vgm(cur_idx()) ? vgm_loop_count() : mdx_loop_count();
}

/* ---- 停止 -------------------------------------------------------------- */

/*
 * 鳴っている方を止める。**出力ゲインには触らない。**
 *
 * ゲインを戻すのは次の曲を始める直前だけにしてある。フェードが終わった時点で
 * 戻すと、リングにまだ残っている「キーオフ前の全音量の区間」がそのまま出て
 * 十数 ms のノイズになる（キーオフはチップに効くだけで、取り込み済みの
 * フレームには遡って効かない）。曲間はゲインを 0 のままにしておく。
 *
 * mdx_state() で囲むのは、MDX_ENABLED=0 のスタブの mdx_stop() が hint を出して
 * "unsupported" を返すため（vgm_play() が同じ理由で同じことをしている）。
 */
static void stop_playback(void) {
    if (vgm_state() != VGM_STATE_STOPPED) {
        vgm_stop();
    }
    if (mdx_state() != MDX_STATE_STOPPED) {
        mdx_stop();
    }
}

static void stop_internal(void) {
    stop_playback();
    /* 自動再生から降りるので、この後の手動再生が絞られたままにならないよう戻す。 */
    ym3012_fade_clear();
    s_state = AUTOPLAY_STOPPED;
}

/* ---- 曲順 -------------------------------------------------------------- */

static void shuffle(void) {
    for (uint32_t i = s_count; i > 1u; i--) {
        uint32_t j = get_rand_32() % i;
        uint16_t t = s_order[i - 1u];
        s_order[i - 1u] = s_order[j];
        s_order[j] = t;
    }
    /* 一巡の切れ目で同じ曲が 2 回続かないようにする */
    if (s_count > 1u && s_order[0] == s_prev_idx) {
        uint16_t t = s_order[0];
        s_order[0] = s_order[1];
        s_order[1] = t;
    }
}

/* dir >= 0 で次、負で前。前方へ一巡したら random では並べ直す。 */
static void advance(int dir) {
    if (s_count == 0u) {
        return;
    }
    if (dir >= 0) {
        s_pos++;
        if (s_pos >= s_count) {
            s_pos = 0;
            if (s_mode == AUTOPLAY_MODE_RANDOM) {
                shuffle();
            }
        }
    } else {
        s_pos = (s_pos == 0u) ? (s_count - 1u) : (s_pos - 1u);
    }
}

/* ---- 曲を始める -------------------------------------------------------- */

/*
 * 現在位置から鳴らせる曲が見つかるまで送る。成功なら NULL。
 *
 * 開けない曲を握り潰さずに 1 曲ずつ理由を出す。キャプチャ中 (`p 1`) に φM の違う曲へ
 * 進むと clockmode_follow_file() が拒否するので、この経路は普通に通る。
 * 全部だめなら自動再生ごと止める。
 */
static const char *start_track(void) {
    const char *err = "not found";

    for (uint32_t tried = 0; tried < s_count; tried++) {
        uint32_t idx = cur_idx();
        const char *name = idx_name(idx);

        ym3012_fade_clear();
        s_prev_idx = (uint16_t)idx;

        err = idx_is_vgm(idx) ? vgm_play(name) : mdx_play(name);
        if (err == NULL) {
            s_state = AUTOPLAY_PLAYING;
            return NULL;
        }

        printf("# autoplay: skip %s (%s)\n", name, err);
        advance(1);
    }

    printf("# autoplay: stopped (no playable file)\n");
    stop_internal();
    return err;
}

static void begin_gap(void) {
    stop_playback(); /* ゲインは 0 のまま。戻すのは start_track() */
    s_gap_end_us = time_us_64() + (uint64_t)s_gap_ms * 1000ull;
    s_state = AUTOPLAY_GAP;
}

static void begin_fade(void) {
    /*
     * フレームレートは φM/64。i2s_rate_hz() ではなくこちらから引くのは、
     * I2S_ENABLED=0 の構成でも同じ値が要るため。
     */
    uint32_t rate = opm_clock_hz_actual() / 64u;
    uint32_t frames = (uint32_t)(((uint64_t)s_fade_ms * (uint64_t)rate) / 1000ull);

    /*
     * 起点はいま取り込み終えたフレーム。消費者はここより後ろを読んでいるので、
     * フェードは取りこぼしなく全部に掛かる。
     */
    ym3012_fade_start(ym3012_write_total(), frames);
    s_fade_end_us = time_us_64() + (uint64_t)s_fade_ms * 1000ull;
    s_state = AUTOPLAY_FADING;

    printf("# autoplay: fade out %s\n", idx_name(cur_idx()));
}

/* ---- 初期化とサービス -------------------------------------------------- */

void autoplay_init(void) {
    s_mode = AUTOPLAY_MODE_LIST;
    s_source = AUTOPLAY_SOURCE_BOTH;
    s_loop = AUTOPLAY_LOOP_DEFAULT;
    s_fade_ms = AUTOPLAY_FADE_MS_DEFAULT;
    s_gap_ms = AUTOPLAY_GAP_MS_DEFAULT;
    s_state = AUTOPLAY_STOPPED;
    s_count = 0;
    s_vgm_count = 0;
    s_pos = 0;
    s_prev_idx = 0xffffu;
    s_busy = false;
}

bool autoplay_service(void) {
    if (s_state == AUTOPLAY_STOPPED || s_busy) {
        return false;
    }

    s_busy = true;
    bool did = false;

    switch (s_state) {
    case AUTOPLAY_PLAYING:
        if (!cur_is_playing()) {
            /* 曲が終わったか、途中で壊れて ERROR に落ちた。どちらも次へ送る。 */
            begin_gap();
            did = true;
        } else if (s_loop != 0u && cur_loops() >= s_loop) {
            if (s_fade_ms == 0u) {
                begin_gap();
            } else {
                begin_fade();
            }
            did = true;
        }
        break;

    case AUTOPLAY_FADING:
        /*
         * 期限は壁時計で見る。I2S の先行は 1024 フレーム（約 16ms）しかなく、
         * その時点で鳴らし残しているのはゲインがほぼ 0 の区間なので足りる。
         * 曲の方が先に終わったときもここで拾う。
         */
        if (time_us_64() >= s_fade_end_us || !cur_is_playing()) {
            begin_gap();
            did = true;
        }
        break;

    case AUTOPLAY_GAP:
        if (time_us_64() >= s_gap_end_us) {
            advance(1);
            start_track();
            did = true;
        }
        break;

    case AUTOPLAY_STOPPED:
    default:
        break;
    }

    s_busy = false;
    return did;
}

/* ---- 操作 -------------------------------------------------------------- */

const char *autoplay_start(void) {
    if (!storage_fatfs_may_access()) {
        printf("# hint    : the filesystem is handed to the PC; run storage player first\n");
        return "wrong state";
    }
    if (storage_fs_state() != STORAGE_FS_MOUNTED) {
        return "no filesystem";
    }

    stop_internal();

    filelist_buf_t buf;
    buf.pool = s_pool;
    buf.pool_bytes = (uint32_t)sizeof(s_pool);
    buf.offs = s_offs;
    buf.max_entries = AUTOPLAY_MAX_ENTRIES;
    filelist_buf_reset(&buf);

    s_count = 0;
    s_vgm_count = 0;
    s_pos = 0;
    s_prev_idx = 0xffffu;

    s_busy = true; /* 集めている間は曲送りを動かさない */

    const char *vgm_err = NULL;
    const char *mdx_err = NULL;

    if (s_source != AUTOPLAY_SOURCE_MDX) {
        vgm_err = vgm_collect(&buf);
    }
    s_vgm_count = buf.count;

#if MDX_ENABLED
    if (s_source != AUTOPLAY_SOURCE_VGM) {
        mdx_err = mdx_collect(&buf);
    }
#endif

    s_count = buf.count;
    s_busy = false;

    if (s_count == 0u) {
        if (vgm_err != NULL) {
            return vgm_err;
        }
        if (mdx_err != NULL) {
            return mdx_err;
        }
        return "not found";
    }

    /* 片方だけ集まったときは続ける。読めなかった側は理由だけ出す。 */
    if (vgm_err != NULL) {
        printf("# warn    : %s: %s\n", VGM_DIR, vgm_err);
    }
    if (mdx_err != NULL) {
        printf("# warn    : %s: %s\n", MDX_DIR, mdx_err);
    }

    for (uint32_t i = 0; i < s_count; i++) {
        s_order[i] = (uint16_t)i;
    }
    if (s_mode == AUTOPLAY_MODE_RANDOM) {
        shuffle();
    }

    /* 開始時は間隔を空けない */
    return start_track();
}

const char *autoplay_stop(void) {
    if (s_state == AUTOPLAY_STOPPED) {
        return NULL;
    }
    stop_internal();
    printf("# autoplay: stopped\n");
    return NULL;
}

const char *autoplay_skip(int dir) {
    if (s_state == AUTOPLAY_STOPPED) {
        printf("# hint    : autoplay is not running; run autoplay start first\n");
        return "wrong state";
    }
    stop_playback();
    advance(dir);
    return start_track();
}

/* ---- 設定 -------------------------------------------------------------- */

void autoplay_set_mode(autoplay_mode_t mode) {
    if (s_mode == mode) {
        return;
    }
    s_mode = mode;
    /* 走っている最中でも次の並びから効かせる。いまの曲は止めない。 */
    if (s_count != 0u) {
        for (uint32_t i = 0; i < s_count; i++) {
            s_order[i] = (uint16_t)i;
        }
        if (mode == AUTOPLAY_MODE_RANDOM) {
            shuffle();
        }
        /* 鳴っている曲を s_order 上で見失わないよう、位置を引き直す。 */
        s_pos = 0;
        if (s_prev_idx != 0xffffu) {
            for (uint32_t i = 0; i < s_count; i++) {
                if (s_order[i] == s_prev_idx) {
                    s_pos = i;
                    break;
                }
            }
        }
    }
}

void autoplay_set_source(autoplay_source_t source) {
    s_source = source; /* 次の autoplay start から効く */
}

void autoplay_set_loop(uint32_t loops) {
    s_loop = loops;
}

void autoplay_set_fade_ms(uint32_t ms) {
    s_fade_ms = ms;
}

void autoplay_set_gap_ms(uint32_t ms) {
    s_gap_ms = ms;
}

autoplay_mode_t autoplay_mode(void) {
    return s_mode;
}

autoplay_source_t autoplay_source(void) {
    return s_source;
}

uint32_t autoplay_loop(void) {
    return s_loop;
}

uint32_t autoplay_fade_ms(void) {
    return s_fade_ms;
}

uint32_t autoplay_gap_ms(void) {
    return s_gap_ms;
}

/* ---- 問い合わせ -------------------------------------------------------- */

autoplay_state_t autoplay_state(void) {
    return s_state;
}

const char *autoplay_state_name(void) {
    switch (s_state) {
    case AUTOPLAY_PLAYING:
        return "PLAYING";
    case AUTOPLAY_FADING:
        return "FADING";
    case AUTOPLAY_GAP:
        return "GAP";
    case AUTOPLAY_STOPPED:
    default:
        return "STOPPED";
    }
}

bool autoplay_is_running(void) {
    return s_state != AUTOPLAY_STOPPED;
}

const char *autoplay_mode_name(void) {
    return (s_mode == AUTOPLAY_MODE_RANDOM) ? "random" : "list";
}

const char *autoplay_source_name(void) {
    switch (s_source) {
    case AUTOPLAY_SOURCE_VGM:
        return "vgm";
    case AUTOPLAY_SOURCE_MDX:
        return "mdx";
    case AUTOPLAY_SOURCE_BOTH:
    default:
        return "both";
    }
}

uint32_t autoplay_count(void) {
    return s_count;
}

uint32_t autoplay_position(void) {
    return (s_state == AUTOPLAY_STOPPED || s_count == 0u) ? 0u : (s_pos + 1u);
}

const char *autoplay_current_name(void) {
    if (s_state == AUTOPLAY_STOPPED || s_count == 0u) {
        return "";
    }
    return idx_name(cur_idx());
}

bool autoplay_current_is_vgm(void) {
    if (s_count == 0u) {
        return false;
    }
    return idx_is_vgm(cur_idx());
}

const char *autoplay_print_list(void (*tick)(void)) {
    if (s_count == 0u) {
        printf("# hint    : the playlist is empty; run autoplay start first\n");
        return "not found";
    }

    s_busy = true; /* 出している間に曲送りで並びが変わらないようにする */

    for (uint32_t i = 0; i < s_count; i++) {
        uint32_t idx = s_order[i];
        bool cur = (s_state != AUTOPLAY_STOPPED) && (i == s_pos);
        printf("# entry   : %c %3u %s %s\n", cur ? '*' : ' ', (unsigned)(i + 1u),
               idx_is_vgm(idx) ? "vgm" : "mdx", idx_name(idx));
        if (tick != NULL) {
            tick();
        }
    }
    printf("# entries : %u\n", (unsigned)s_count);

    s_busy = false;
    return NULL;
}

#else /* !AUTOPLAY_ENABLED */

/* 無効ビルドであることを応答から判別できるように、理由を出して unsupported を返す。 */
static const char *autoplay_disabled(void) {
    printf("# hint    : autoplay is disabled at build time (AUTOPLAY_ENABLED=0)\n");
    return "unsupported";
}

void autoplay_init(void) {}
bool autoplay_service(void) { return false; }
const char *autoplay_start(void) { return autoplay_disabled(); }
const char *autoplay_stop(void) { return NULL; }
const char *autoplay_skip(int dir) { (void)dir; return autoplay_disabled(); }
void autoplay_set_mode(autoplay_mode_t mode) { (void)mode; }
void autoplay_set_source(autoplay_source_t source) { (void)source; }
void autoplay_set_loop(uint32_t loops) { (void)loops; }
void autoplay_set_fade_ms(uint32_t ms) { (void)ms; }
void autoplay_set_gap_ms(uint32_t ms) { (void)ms; }
autoplay_mode_t autoplay_mode(void) { return AUTOPLAY_MODE_LIST; }
autoplay_source_t autoplay_source(void) { return AUTOPLAY_SOURCE_BOTH; }
uint32_t autoplay_loop(void) { return 0; }
uint32_t autoplay_fade_ms(void) { return 0; }
uint32_t autoplay_gap_ms(void) { return 0; }
autoplay_state_t autoplay_state(void) { return AUTOPLAY_STOPPED; }
const char *autoplay_state_name(void) { return "DISABLED"; }
bool autoplay_is_running(void) { return false; }
const char *autoplay_mode_name(void) { return "list"; }
const char *autoplay_source_name(void) { return "both"; }
uint32_t autoplay_count(void) { return 0; }
uint32_t autoplay_position(void) { return 0; }
const char *autoplay_current_name(void) { return ""; }
bool autoplay_current_is_vgm(void) { return false; }
const char *autoplay_print_list(void (*tick)(void)) { (void)tick; return autoplay_disabled(); }

#endif /* AUTOPLAY_ENABLED */
