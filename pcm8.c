/*
 * PCM8 相当の ADPCM ソフトデコードとミキシングの実装
 *
 * SPDX-License-Identifier: MIT
 */
#include "pcm8.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "ff.h"

#include "stats.h"
#include "storage.h"
#include "ym3012.h"

#if PCM8_ENABLED

/* ---- 定数 -------------------------------------------------------------- */

/*
 * ミックスリング。キャプチャ側 (ym3012.c) と同じ幾何にする。
 *
 * USB キャプチャと I2S はそれぞれ独立したカーソルを持ち、読み出し位置が違う。
 * どちらの位置から読まれても同じ内容を返せるように、フレーム番号で引ける
 * リングに描いておき、読み出しでは消費しない（= 冪等）。カーソルは
 * 書き込み位置から最大でリング 1 周ぶんしか離れないので、この長さで足りる。
 */
#define PCM8_MIX_FRAMES YM3012_RING_FRAMES

/* 1 回のレンダリングで扱うフレーム数。int32 の蓄積バッファの大きさ。 */
#define PCM8_BLOCK_FRAMES 256u

/*
 * 発音中にまとめて描く最小フレーム数。
 *
 * メインループは 20 万周/秒ほど回るのに対して出力は 62500 フレーム/秒しかないので、
 * 毎周回描くと 1 周あたり 1 フレーム未満のために毎回ブロックの支度をすることになり、
 * 固定費だけで CPU の 2 割近くを使う。64 フレーム (約 1ms) 溜めてから描けば
 * 固定費は 1/60 以下になる。消費者は ym3012_set_mix_ready() で待つ。
 */
#define PCM8_BATCH_FRAMES 64u

/*
 * モード -> 1 ソースサンプルあたりの出力フレーム数。
 *
 * 出力は φM/64、ADPCM は φM/1024, /768, /512, /384, /256 なので、
 * φM の値によらず比は整数になる。5 (16bit PCM) と 6 (8bit PCM) は
 * 15.6kHz 固定なので mode 4 と同じ。
 */
static const uint8_t HOLD_FOR_MODE[7] = {16u, 12u, 8u, 6u, 4u, 4u, 4u};

/*
 * PCM8 の音量 0-15 -> 線形ゲイン (Q16)。1step = 2dB で 8 が原音。
 * gain[v] = round(65536 * 10^((v - 8) / 10))
 */
static const int32_t GAIN_Q16[16] = {
    10387,  13076,  16462,  20724,  26090,  32846,  41350,  52057,
    65536,  82505,  103868, 130762, 164619, 207243, 260904, 328458,
};

/* MSM6258V の ADPCM。ステップ番号の増減とステップ幅。 */
static const int8_t STEP_ADJ[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static const int16_t STEP_SIZE[49] = {
    16,   17,   19,   21,   23,   25,   28,   31,   34,   37,   41,   45,  50,
    55,   60,   66,   73,   80,   88,   97,   107,  118,  130,  143,  157, 173,
    190,  209,  230,  253,  279,  307,  337,  371,  408,  449,  494,  544, 598,
    658,  724,  796,  876,  963,  1060, 1166, 1282, 1411, 1552,
};

/* ---- チャンネル -------------------------------------------------------- */

typedef struct {
    bool active;

    /* PDX 上の読み出し範囲 */
    uint32_t data_pos; /* 次に読むファイル位置 */
    uint32_t data_end; /* offset + length */

    /* 先読み */
    uint8_t buf[PCM8_PREFETCH_BYTES];
    uint32_t buf_len;
    uint32_t buf_pos;

    /* 形式と音量。停止しても保持する（PCM8 は -1 で据え置きにできる） */
    uint8_t mode;
    uint8_t vol;

    /* レート変換。補間はせず hold 回だけ同じ値を繰り返す。 */
    uint8_t hold;
    uint8_t hold_cnt;

    /* ADPCM のデコード状態 */
    int32_t sig;
    int32_t idx;
    bool nib_high;
    uint8_t nib_byte;

    /* ゲインを掛けた現在のサンプル */
    int32_t cur;
} pcm8_ch_t;

static pcm8_ch_t s_ch[PCM8_CH_MAX];

/* ---- 全体の状態 -------------------------------------------------------- */

static bool s_enabled = true;

/* 定位は PCM8 の仕様どおり全チャネル共通。0 以外で最後に指定された値が残る。 */
static uint32_t s_pan = 3u;

static uint32_t s_mix[PCM8_MIX_FRAMES] __attribute__((aligned(4)));
static uint64_t s_rendered_total;
static uint32_t s_quiet_frames; /* 末尾から連続して無音なフレーム数 */
static bool s_sounding;         /* 直前のレンダリングで発音していたか */
static uint64_t s_clip_scanned; /* 飽和を数え終わったフレーム番号（二重計上を防ぐ） */

static int32_t s_acc[PCM8_BLOCK_FRAMES];

static uint32_t s_reads;
static uint32_t s_keyon;
static uint32_t s_miss;

/* ---- PDX --------------------------------------------------------------- */

static FIL s_pdx_fp;
static bool s_pdx_open;
static uint32_t s_pdx_size;
static char s_pdx_path[80];

/* エントリ表は 1 バンク分だけ持つ。バンクが変わったときに読み直す。 */
static uint8_t s_tbl[PDX_BANK_BYTES];
static int32_t s_tbl_bank = -1;

static uint32_t rd32be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
           (uint32_t)p[3];
}

/* (bank, note) のサンプル位置を引く。無い / 壊れているなら false。 */
static bool pdx_entry(uint32_t bank, uint32_t note, uint32_t *off, uint32_t *len) {
    if (!s_pdx_open || note >= PDX_BANK_NOTES) {
        return false;
    }

    if ((int32_t)bank != s_tbl_bank) {
        FSIZE_t at = (FSIZE_t)bank * PDX_BANK_BYTES;
        if (at + PDX_BANK_BYTES > s_pdx_size) {
            return false;
        }
        if (f_lseek(&s_pdx_fp, at) != FR_OK) {
            return false;
        }
        UINT got = 0;
        if (f_read(&s_pdx_fp, s_tbl, PDX_BANK_BYTES, &got) != FR_OK ||
            got != PDX_BANK_BYTES) {
            s_tbl_bank = -1;
            return false;
        }
        s_tbl_bank = (int32_t)bank;
        s_reads++;
    }

    const uint8_t *e = &s_tbl[note * PDX_ENTRY_BYTES];
    uint32_t o = rd32be(e);
    uint32_t n = rd32be(e + 4) & 0x00ffffffu; /* 参照実装が長さを 24bit で切る */

    if (n == 0u || o >= s_pdx_size || (o + n) > s_pdx_size) {
        return false;
    }
    *off = o;
    *len = n;
    return true;
}

/* ---- サンプルの取り出し ------------------------------------------------ */

static bool refill(pcm8_ch_t *c) {
    if (!s_pdx_open || c->data_pos >= c->data_end) {
        return false;
    }
    uint32_t want = c->data_end - c->data_pos;
    if (want > PCM8_PREFETCH_BYTES) {
        want = PCM8_PREFETCH_BYTES;
    }
    if (f_lseek(&s_pdx_fp, (FSIZE_t)c->data_pos) != FR_OK) {
        return false;
    }
    UINT got = 0;
    if (f_read(&s_pdx_fp, c->buf, (UINT)want, &got) != FR_OK || got == 0u) {
        return false;
    }
    c->buf_len = got;
    c->buf_pos = 0;
    c->data_pos += got;
    s_reads++;
    return true;
}

static int next_byte(pcm8_ch_t *c) {
    if (c->buf_pos >= c->buf_len) {
        if (!refill(c)) {
            return -1;
        }
    }
    return (int)c->buf[c->buf_pos++];
}

/*
 * MSM6258V の ADPCM を 1 ニブル進める。
 *   delta = step/8 + step/4 * b0 + step/2 * b1 + step * b2、符号は b3
 *   出力は 12bit で頭打ち、ステップ番号は 0-48 で頭打ち
 */
static int32_t adpcm_step(int32_t *sigp, int32_t *idxp, uint8_t nib) {
    int32_t step = STEP_SIZE[*idxp];
    int32_t d = step >> 3;
    if ((nib & 1u) != 0u) {
        d += step >> 2;
    }
    if ((nib & 2u) != 0u) {
        d += step >> 1;
    }
    if ((nib & 4u) != 0u) {
        d += step;
    }
    if ((nib & 8u) != 0u) {
        d = -d;
    }

    int32_t sig = *sigp + d;
    if (sig > 2047) {
        sig = 2047;
    } else if (sig < -2048) {
        sig = -2048;
    }
    *sigp = sig;

    int32_t idx = *idxp + STEP_ADJ[nib & 7u];
    if (idx > 48) {
        idx = 48;
    } else if (idx < 0) {
        idx = 0;
    }
    *idxp = idx;

    /* 12bit を 16bit フルスケールへ。16bit PCM 形式と同じレベルになる。 */
    return sig << 4;
}

/* 次のソースサンプルを取り出してゲインを掛ける。尽きたら false。 */
static bool next_sample(pcm8_ch_t *c) {
    int32_t raw;

    if (c->mode <= 4u) {
        uint8_t nib;
        if (!c->nib_high) {
            int b = next_byte(c);
            if (b < 0) {
                return false;
            }
            c->nib_byte = (uint8_t)b;
            nib = (uint8_t)((uint32_t)b & 0x0fu); /* 下位ニブルが先 */
            c->nib_high = true;
        } else {
            nib = (uint8_t)(c->nib_byte >> 4);
            c->nib_high = false;
        }
        raw = adpcm_step(&c->sig, &c->idx, nib);
    } else if (c->mode == 5u) {
        int hi = next_byte(c);
        if (hi < 0) {
            return false;
        }
        int lo = next_byte(c);
        if (lo < 0) {
            return false;
        }
        raw = (int32_t)(int16_t)(uint16_t)(((uint32_t)hi << 8) | (uint32_t)lo);
    } else {
        int b = next_byte(c);
        if (b < 0) {
            return false;
        }
        raw = (int32_t)(int8_t)(uint8_t)b << 8;
    }

    /* ゲインはソースサンプル 1 個につき 1 回だけ掛ける（出力フレームごとではない） */
    c->cur = (int32_t)(((int64_t)raw * (int64_t)GAIN_Q16[c->vol]) >> 16);
    c->hold_cnt = c->hold;
    return true;
}

/* ---- レンダリング ------------------------------------------------------ */

static void mix_channel(pcm8_ch_t *c, int32_t *acc, uint32_t n) {
    uint32_t i = 0;
    while (i < n) {
        if (c->hold_cnt == 0u) {
            if (!next_sample(c)) {
                c->active = false;
                return;
            }
        }
        uint32_t run = c->hold_cnt;
        if (run > n - i) {
            run = n - i;
        }
        int32_t v = c->cur;
        for (uint32_t k = 0; k < run; k++) {
            acc[i + k] += v;
        }
        c->hold_cnt = (uint8_t)(c->hold_cnt - run);
        i += run;
    }
}

static bool render_block(uint32_t widx, uint32_t n) {
    uint32_t active = 0;

    memset(s_acc, 0, (size_t)n * sizeof(s_acc[0]));

    for (uint32_t i = 0; i < PCM8_CH_MAX; i++) {
        pcm8_ch_t *c = &s_ch[i];
        if (!c->active) {
            continue;
        }
        active++;
        mix_channel(c, s_acc, n);
    }

    if (active == 0u) {
        if (s_quiet_frames >= PCM8_MIX_FRAMES) {
            /* リング全体が既に無音。ゼロで上書きし直す必要はない。 */
            return false;
        }
        s_quiet_frames += n;
        if (s_quiet_frames > PCM8_MIX_FRAMES) {
            s_quiet_frames = PCM8_MIX_FRAMES;
        }
        /* 無音になった直後のぶんは、古い内容を消すためにゼロを書く */
    } else {
        s_quiet_frames = 0;
    }

    /*
     * 8ch を 32bit で合算し、最後に 1 回だけ飽和させる。
     * チャンネルごとに 16bit へ飽和させると歪む。
     */
    uint32_t pan = s_pan;
    uint32_t clip = 0;
    for (uint32_t i = 0; i < n; i++) {
        int32_t v = s_acc[i];
        if (v > 32767) {
            v = 32767;
            clip++;
        } else if (v < -32768) {
            v = -32768;
            clip++;
        }
        uint16_t w = (uint16_t)(int16_t)v;
        uint16_t l = ((pan & 1u) != 0u) ? w : 0u;
        uint16_t r = ((pan & 2u) != 0u) ? w : 0u;
        s_mix[widx + i] = ((uint32_t)r << 16) | (uint32_t)l;
    }
    if (clip != 0u) {
        stats_count_pcm_clip(clip);
    }

    return active != 0u;
}

/* ---- ミキサフック ------------------------------------------------------ */

static inline int16_t sat16(int32_t v, uint32_t *clip) {
    if (v > 32767) {
        (*clip)++;
        return 32767;
    }
    if (v < -32768) {
        (*clip)++;
        return -32768;
    }
    return (int16_t)v;
}

/*
 * ym3012_reader_read_pcm() から呼ばれる。first_frame は消費者によらない
 * 絶対フレーム番号なので、カーソルが 2 本あってもここは冪等でよい。
 */
static void pcm8_mix(uint64_t first_frame, uint32_t frames, int16_t *inout) {
    if (!s_enabled || s_quiet_frames >= PCM8_MIX_FRAMES) {
        return;
    }

    uint64_t start = first_frame;
    uint64_t end = first_frame + frames;

    if (start >= s_rendered_total) {
        return; /* まだ描いていない */
    }
    if (end > s_rendered_total) {
        end = s_rendered_total;
    }
    uint64_t oldest =
        (s_rendered_total > PCM8_MIX_FRAMES) ? (s_rendered_total - PCM8_MIX_FRAMES) : 0u;
    if (start < oldest) {
        start = oldest; /* すでに上書きされている */
    }
    if (start >= end) {
        return;
    }

    /* 内側は 32bit だけで回す（1 フレームあたりの手数を増やさない） */
    uint32_t n = (uint32_t)(end - start);
    uint32_t ridx = (uint32_t)(start & (PCM8_MIX_FRAMES - 1u));
    int16_t *out = &inout[2u * (uint32_t)(start - first_frame)];

    /* 飽和は絶対フレーム番号で 1 回だけ数える（カーソル 2 本で二重計上しない） */
    uint32_t skip = (start < s_clip_scanned) ? (uint32_t)(s_clip_scanned - start) : 0u;
    if (skip > n) {
        skip = n;
    }
    uint32_t clip = 0;
    uint32_t discard = 0;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t m = s_mix[ridx];
        ridx = (ridx + 1u) & (PCM8_MIX_FRAMES - 1u);

        uint32_t *cp = (i >= skip) ? &clip : &discard;

        int32_t l = (int32_t)out[0] + (int32_t)(int16_t)(uint16_t)m;
        int32_t r = (int32_t)out[1] + (int32_t)(int16_t)(uint16_t)(m >> 16);

        out[0] = sat16(l, cp);
        out[1] = sat16(r, cp);
        out += 2;
    }

    if (end > s_clip_scanned) {
        s_clip_scanned = end;
    }
    if (clip != 0u) {
        stats_count_pcm_clip(clip);
    }
}

/* ---- 初期化とサービス -------------------------------------------------- */

void pcm8_init(void) {
    memset(s_ch, 0, sizeof(s_ch));
    for (uint32_t i = 0; i < PCM8_CH_MAX; i++) {
        s_ch[i].mode = 4u; /* 15.6kHz。MXDRV のチャンネル初期値 (pan = 0x10) と同じ */
        s_ch[i].vol = 8u;  /* 原音量 */
        s_ch[i].hold = HOLD_FOR_MODE[4];
    }
    memset(s_mix, 0, sizeof(s_mix));

    s_enabled = true;
    s_pan = 3u;
    s_reads = 0;
    s_keyon = 0;
    s_miss = 0;
    s_tbl_bank = -1;
    s_pdx_open = false;
    s_pdx_size = 0;
    s_pdx_path[0] = '\0';

    s_rendered_total = ym3012_write_total();
    s_clip_scanned = s_rendered_total;
    s_quiet_frames = PCM8_MIX_FRAMES;
    s_sounding = false;

    ym3012_set_mixer(pcm8_mix);
    ym3012_set_mix_ready(s_rendered_total);
}

bool pcm8_service(void) {
    uint64_t w = ym3012_write_total();
    if (s_rendered_total >= w) {
        return false;
    }

    uint64_t behind = w - s_rendered_total;
    if (behind > PCM8_MIX_FRAMES) {
        /*
         * リング 1 周ぶんより遅れた（フラッシュ書き込みなどでメインループが
         * 止まった）。追いつこうとしても読まれない領域を描くだけなので、
         * 無音で埋めて位置を張り直す。
         */
        pcm8_resync();
        return false;
    }

    /*
     * 鳴っているあいだはある程度まとめて描く。無音のときはブロックの支度そのものを
     * しないので（リング全体が既に 0）、束ねずに毎回進めてよい。
     */
    if (s_sounding && behind < PCM8_BATCH_FRAMES) {
        return false;
    }

    bool sounded = false;
    while (s_rendered_total < w) {
        uint32_t widx = (uint32_t)(s_rendered_total & (PCM8_MIX_FRAMES - 1u));
        uint32_t n = PCM8_MIX_FRAMES - widx; /* リング末尾はまたがない */
        uint64_t remain = w - s_rendered_total;
        if ((uint64_t)n > remain) {
            n = (uint32_t)remain;
        }
        if (n > PCM8_BLOCK_FRAMES) {
            n = PCM8_BLOCK_FRAMES;
        }
        sounded |= render_block(widx, n);
        s_rendered_total += n;
    }

    s_sounding = sounded;
    ym3012_set_mix_ready(s_rendered_total);

    if (s_rendered_total > PCM8_MIX_FRAMES &&
        s_clip_scanned < s_rendered_total - PCM8_MIX_FRAMES) {
        s_clip_scanned = s_rendered_total - PCM8_MIX_FRAMES;
    }
    return sounded;
}

void pcm8_resync(void) {
    memset(s_mix, 0, sizeof(s_mix));
    s_quiet_frames = PCM8_MIX_FRAMES;
    s_sounding = false;
    s_rendered_total = ym3012_write_total();
    s_clip_scanned = s_rendered_total;
    ym3012_set_mix_ready(s_rendered_total);
}

/* ---- PDX --------------------------------------------------------------- */

const char *pcm8_open_pdx(const char *path) {
    pcm8_close_pdx();

    if (!storage_fatfs_may_access() || storage_fs_state() != STORAGE_FS_MOUNTED) {
        return "wrong state";
    }

    FRESULT fr = f_open(&s_pdx_fp, path, FA_READ);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
        return "not found";
    }
    if (fr != FR_OK) {
        return "io error";
    }

    FSIZE_t size = f_size(&s_pdx_fp);
    if (size < PDX_BANK_BYTES) {
        f_close(&s_pdx_fp);
        return "bad file";
    }

    s_pdx_size = (uint32_t)size;
    s_pdx_open = true;
    s_tbl_bank = -1;
    pcm8_reset_counters();
    snprintf(s_pdx_path, sizeof(s_pdx_path), "%s", path);
    return NULL;
}

void pcm8_close_pdx(void) {
    pcm8_abort_all();
    if (s_pdx_open) {
        f_close(&s_pdx_fp);
        s_pdx_open = false;
    }
    s_pdx_size = 0;
    s_tbl_bank = -1;
    s_pdx_path[0] = '\0';
}

bool pcm8_pdx_ready(void) {
    return s_pdx_open;
}

const char *pcm8_pdx_path(void) {
    return s_pdx_path;
}

/* ---- 発音 -------------------------------------------------------------- */

/* 音量/周波数/定位の適用。負値は据え置き。pan == 0 はそのチャネルの停止。 */
static bool apply_params(pcm8_ch_t *c, int mode, int vol, int pan) {
    if (mode >= 0) {
        uint32_t m = (uint32_t)mode;
        if (m > 6u) {
            m = 6u;
        }
        c->mode = (uint8_t)m;
        c->hold = HOLD_FOR_MODE[m];
    }
    if (vol >= 0) {
        c->vol = (uint8_t)((uint32_t)vol & 0x0fu);
    }
    if (pan >= 0) {
        if (pan == 0) {
            return false; /* 停止。定位は変更しない */
        }
        s_pan = (uint32_t)pan & 0x03u;
    }
    return true;
}

void pcm8_key_on(uint32_t ch, uint32_t bank, uint32_t note, int mode, int vol, int pan) {
    if (ch >= PCM8_CH_MAX) {
        return;
    }
    pcm8_ch_t *c = &s_ch[ch];

    bool sound = apply_params(c, mode, vol, pan);
    if (!sound) {
        s_miss++;
        c->active = false;
        return;
    }

    uint32_t off = 0;
    uint32_t len = 0;
    if (!pdx_entry(bank, note, &off, &len)) {
        s_miss++;
        c->active = false;
        return;
    }

    c->data_pos = off;
    c->data_end = off + len;
    c->buf_len = 0;
    c->buf_pos = 0;
    c->sig = 0;
    c->idx = 0;
    c->nib_high = false;
    c->hold_cnt = 0;
    c->cur = 0;
    c->active = true;
    s_keyon++;
}

void pcm8_set_mode(uint32_t ch, int vol, int mode, int pan) {
    if (ch >= PCM8_CH_MAX) {
        return;
    }
    pcm8_ch_t *c = &s_ch[ch];
    if (!apply_params(c, mode, vol, pan)) {
        c->active = false;
        return;
    }
    /* 発音中なら次のソースサンプルから新しい音量とレートが効く */
}

void pcm8_stop(uint32_t ch) {
    if (ch >= PCM8_CH_MAX) {
        return;
    }
    s_ch[ch].active = false;
}

void pcm8_end_all(void) {
    /*
     * $0100 は「チェイン動作を解除し、出力中のデータブロックの出力が完了次第停止」。
     * チェインは使っていないので、鳴っているぶんは最後まで鳴らして終わる
     * = ここでは何もしない。
     */
}

void pcm8_abort_all(void) {
    for (uint32_t i = 0; i < PCM8_CH_MAX; i++) {
        s_ch[i].active = false;
    }
}

/* ---- 問い合わせ -------------------------------------------------------- */

uint32_t pcm8_active_mask(void) {
    uint32_t m = 0;
    for (uint32_t i = 0; i < PCM8_CH_MAX; i++) {
        if (s_ch[i].active) {
            m |= 1u << i;
        }
    }
    return m;
}

uint32_t pcm8_active_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < PCM8_CH_MAX; i++) {
        if (s_ch[i].active) {
            n++;
        }
    }
    return n;
}

uint32_t pcm8_pan(void) {
    return s_pan;
}

uint32_t pcm8_read_count(void) {
    return s_reads;
}

uint32_t pcm8_keyon_count(void) {
    return s_keyon;
}

uint32_t pcm8_miss_count(void) {
    return s_miss;
}

void pcm8_reset_counters(void) {
    s_keyon = 0;
    s_miss = 0;
    s_reads = 0;
}

void pcm8_set_enabled(bool on) {
    s_enabled = on;
}

bool pcm8_enabled(void) {
    return s_enabled;
}

/* ---- 自己テスト -------------------------------------------------------- */

/*
 * デコーダの既知ベクタ。sig と idx を初期値 0 から進め、既定の値になるか見る。
 * 表の値と delta の式、飽和の効き方をまとめて押さえられる。
 */
bool pcm8_selftest(const char **detail) {
    static char msg[80];

    typedef struct {
        const char *nibs;
        int32_t sig;
        int32_t idx;
    } vec_t;

    /* 上位ニブルは使わず 1 ニブルずつ与える。文字列は 16 進 1 桁の並び。 */
    static const vec_t VEC[] = {
        {"7", 30, 8},
        {"77", 93, 16},
        {"777", 229, 24},
        {"7777", 522, 32},
        {"77777", 1153, 40},
        {"777777", 2047, 48},   /* 12bit で頭打ち */
        {"7777777", 2047, 48},  /* ステップ番号も 48 で頭打ち */
        {"00000000", 16, 0},    /* 最小ステップ。番号は 0 で頭打ち */
        {"ffff", -522, 32},     /* bit3 は符号 */
        {"ffff7", 109, 40},
        {"ffffffffffffffffffffffffffffffffffffffff", -2048, 48},
    };

    uint32_t fail = 0;
    uint32_t total = 0;

    for (uint32_t v = 0; v < sizeof(VEC) / sizeof(VEC[0]); v++) {
        int32_t sig = 0;
        int32_t idx = 0;
        for (const char *p = VEC[v].nibs; *p != '\0'; p++) {
            uint8_t n = (uint8_t)((*p <= '9') ? (*p - '0') : (*p - 'a' + 10));
            (void)adpcm_step(&sig, &idx, n);
        }
        total++;
        if (sig != VEC[v].sig || idx != VEC[v].idx) {
            fail++;
        }
    }

    /* レート比。全部整数でなければ補間なしの前提が崩れる。 */
    static const uint8_t EXPECT_HOLD[7] = {16u, 12u, 8u, 6u, 4u, 4u, 4u};
    for (uint32_t m = 0; m < 7u; m++) {
        total++;
        if (HOLD_FOR_MODE[m] != EXPECT_HOLD[m]) {
            fail++;
        }
    }

    /* 音量 8 は原音（ゲイン 1.0）でなければならない */
    total++;
    if (GAIN_Q16[8] != 65536) {
        fail++;
    }

    if (fail == 0u) {
        snprintf(msg, sizeof(msg), "PASS (%u cases)", (unsigned)total);
    } else {
        snprintf(msg, sizeof(msg), "FAIL %u/%u", (unsigned)fail, (unsigned)total);
    }
    *detail = msg;
    return fail == 0u;
}

#else /* !PCM8_ENABLED */

void pcm8_init(void) {}
bool pcm8_service(void) { return false; }
void pcm8_resync(void) {}

const char *pcm8_open_pdx(const char *path) {
    (void)path;
    return "disabled";
}
void pcm8_close_pdx(void) {}
bool pcm8_pdx_ready(void) { return false; }
const char *pcm8_pdx_path(void) { return ""; }

void pcm8_key_on(uint32_t ch, uint32_t bank, uint32_t note, int mode, int vol, int pan) {
    (void)ch;
    (void)bank;
    (void)note;
    (void)mode;
    (void)vol;
    (void)pan;
}
void pcm8_set_mode(uint32_t ch, int vol, int mode, int pan) {
    (void)ch;
    (void)vol;
    (void)mode;
    (void)pan;
}
void pcm8_stop(uint32_t ch) { (void)ch; }
void pcm8_end_all(void) {}
void pcm8_abort_all(void) {}

uint32_t pcm8_active_mask(void) { return 0; }
uint32_t pcm8_active_count(void) { return 0; }
uint32_t pcm8_pan(void) { return 0; }
uint32_t pcm8_read_count(void) { return 0; }
uint32_t pcm8_keyon_count(void) { return 0; }
uint32_t pcm8_miss_count(void) { return 0; }
void pcm8_reset_counters(void) {}
void pcm8_set_enabled(bool on) { (void)on; }
bool pcm8_enabled(void) { return false; }

bool pcm8_selftest(const char **detail) {
    *detail = "SKIP (disabled)";
    return true;
}

#endif /* PCM8_ENABLED */
