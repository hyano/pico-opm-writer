/*
 * MDX (X68000 / MXDRV) ファイルの再生
 *
 * SPDX-License-Identifier: MIT
 *
 * ---- 参照実装との対応 --------------------------------------------------
 *
 * 解釈は MXDRV 2.06+17 に合わせてある。参照実装のチャンネルワーク
 * (MXWORK_CH) のオフセットと、この mdx_ch_t のフィールドの対応:
 *
 *   S0000 Ptr                    -> p           MML の読み出し位置
 *   S0004 voice ptr              -> voice       音色レコード（番号の次）
 *   S0004_b PCM bank             -> pcm_bank
 *   S0008 bend delta             -> bend_delta  ポルタメントの 1 clock 分
 *   S000c bend offset            -> bend
 *   S0010 D                      -> detune      1/64 半音
 *   S0012 note+D                 -> pitch       1/64 半音
 *   S0014 note+D+bend+PitchLFO   -> pitch_out   KC/KF へ落とした値の控え
 *   S0016 flags                  -> f16         F16_* 参照
 *   S0017 flags                  -> f17         F17_* 参照
 *   S0018 ch                     -> ch          bit7 で ADPCM、下位 3bit が PCM ch
 *   S0019 carrier slot           -> carrier
 *   S001a len                    -> len         音符・休符の残り clock
 *   S001b gate                   -> gate        キーオフまでの残り clock
 *   S001c p                      -> pan         上位 2bit が RL、下位 6bit が FL/CON
 *   S001d keyon slot             -> keyon_slot  (スロットマスク << 3) | ch
 *   S001e Q                      -> q           ゲートタイム
 *   S001f Keyon delay            -> keyon_delay
 *   S0020 Keyon delay counter    -> keyon_cnt
 *   S0021 PMS/AMS                -> pms_ams
 *   S0022 v                      -> vol         bit7 で @v(TL 直接)、なければ v0-15
 *   S0023 v last                 -> vol_last
 *   S0024 LFO delay              -> lfo_delay
 *   S0025 LFO delay counter      -> lfo_cnt
 *   S0026..S003e Pitch LFO       -> plfo
 *   S0040..S004e Volume LFO      -> vlfo
 *
 * 参照実装のチャンネルワークは 80 バイトで、FM 8ch + ADPCM 1ch が CHBUF_FM に、
 * PCM8 拡張の残り 7ch が CHBUF_PCM に置かれている。ここでは 16 個の配列 1 本。
 *
 * 整数の幅も参照実装に合わせてある（68000 の桁溢れの仕方まで含めて同じ結果に
 * なるように、符号なしで持って必要なところだけ符号付きへキャストする）。
 *
 * ---- OPM レジスタアクセス ----------------------------------------------
 *
 * 参照実装の L_WRITEOPM は「BUSY を待つ / アドレスとデータを書く / 256 バイトの
 * シャドウに控える / レジスタ 0x1B だけ別に控える」をやっている。本機は
 * データバスが出力専用でステータスを読めないので、BUSY 待ちの代わりに
 * opm_write() の固定ウェイト（アドレス後 5us / データ後 25us）を使う。ここだけが
 * 参照実装との相違点。シャドウは 0x1B の read-modify-write に要るのでそのまま持つ。
 *
 * ---- ADPCM -------------------------------------------------------------
 *
 * 本機に MSM6258 は無いので ADPCM パートは発音しない。ただしシーケンサとしては
 * FM と同じように回す（同期コマンドが ADPCM 側から飛んでくる曲があるため）。
 * 発音の有無は snd_* の出力バックエンドに閉じ込めてあり、将来 PCM8 (8ch) を
 * ソフトデコードして混ぜるときはここへ足す。
 */
#include "mdx.h"

#include <stdio.h>
#include <string.h>

#include "clockmode.h"
#include "ff.h"
#include "opm.h"
#include "pcm8.h"
#include "pico/stdlib.h"
#include "stats.h"
#include "storage.h"
#include "vgm.h"

#if MDX_ENABLED

/* ---- フラグ ------------------------------------------------------------ */

#define F16_KEYON_REQ 0x01u /* 音符を読んだ。次の tick でキーオンする */
#define F16_LFO_SYNC  0x02u /* キーオン時に OPM の LFO をリセットする */
#define F16_LEGATO    0x04u /* 0xF7。キーオフしない */
#define F16_KEYED     0x08u /* いまキーオンしている */
#define F16_NO_KEYOFF 0x10u /* 0xE7 0x03。キーオフを抑止する */
#define F16_PLFO      0x20u /* ピッチ LFO 動作中 */
#define F16_VLFO      0x40u /* 音量 LFO 動作中 */
#define F16_PORTA     0x80u /* ポルタメント動作中 */

#define F17_VOL_DIRTY 0x01u
#define F17_VOICE_REQ 0x02u /* 音色の書き込み待ち（実際に書くのはキーオン時） */
#define F17_PAN_DIRTY 0x04u
#define F17_SYNC_WAIT 0x08u /* 0xEE で同期待ち */

/* ---- 状態 -------------------------------------------------------------- */

static mdx_state_t s_state = MDX_STATE_STOPPED;

/* ファイルは丸ごとここへ載せる。読み終えたらファイルは閉じる。 */
static uint8_t s_file[MDX_MAX_BYTES];
static uint32_t s_size;

static char s_name[64];
static char s_title[MDX_TITLE_MAX];
static char s_pdx[MDX_PDX_MAX];

/* ヘッダ由来 */
static uint32_t s_base;
static uint32_t s_voice_top;
static uint32_t s_nch;
static uint32_t s_mml_top[MDX_CH_MAX];

/* 音色番号 -> レコードの位置（番号バイトの次）。0 は無し。 */
static uint16_t s_voice[256];

/* OPM レジスタのシャドウ（参照実装の OPMBUF 相当） */
static uint8_t s_opm[256];
static uint8_t s_opm_1b;

/* ---- チャンネル -------------------------------------------------------- */

typedef struct {
    uint8_t wave; /* S0026。0 = 停止、1-4 = 波形 */
    uint32_t offset_start;
    uint32_t delta_start;
    uint32_t delta;
    uint32_t offset;
    uint16_t len_cooked;
    uint16_t len;
    uint16_t len_cnt;
} mdx_plfo_t;

typedef struct {
    uint8_t wave; /* S0040 */
    uint16_t delta_start;
    uint16_t delta_cooked;
    uint16_t delta;
    uint16_t offset;
    uint16_t len;
    uint16_t len_cnt;
} mdx_vlfo_t;

typedef struct {
    uint32_t p;
    uint32_t voice;
    uint8_t pcm_bank;

    uint32_t bend_delta;
    uint32_t bend;
    uint16_t detune;
    uint16_t pitch;
    uint16_t pitch_out;

    uint8_t f16;
    uint8_t f17;
    uint8_t ch;
    uint8_t carrier;
    uint8_t len;
    uint8_t gate;
    uint8_t pan;
    uint8_t keyon_slot;
    uint8_t q;
    uint8_t keyon_delay;
    uint8_t keyon_cnt;
    uint8_t pms_ams;
    uint8_t vol;
    uint8_t vol_last;
    uint8_t lfo_delay;
    uint8_t lfo_cnt;

    mdx_plfo_t plfo;
    mdx_vlfo_t vlfo;

} mdx_ch_t;

static mdx_ch_t s_ch[MDX_CH_MAX];

/* ---- 全体の状態（参照実装のグローバルワーク相当） ---------------------- */

static uint8_t s_sync[MDX_CH_MAX]; /* L001df6。0xEF で立ち 0xEE で降りる */

/*
 * 参照実装の TieFlag。ch_len() の入口でそのチャンネルのレガート指定を写す
 * （= 直前の音がタイだったか）。ADPCM の音量を発音中に変えるかの判定に使う。
 */
static bool s_tie_flag;
static uint16_t s_enable;          /* L001e1a。まだ終わっていないチャンネル */
static bool s_pcm8_mode;           /* L001df4。0xE8 で立つと 9-15ch も回る */

/* フェードアウト（L001e14 / L001e15 / L001e17 / L001e1e） */
static uint8_t s_fade_off;
static uint8_t s_fade_flag;
static uint8_t s_fade_on;
static uint16_t s_fade_speed;
static uint16_t s_fade_cnt;

/* ---- スケジューラ ------------------------------------------------------ */

static uint8_t s_tempo;
static uint64_t s_due_q; /* 次の tick の予定時刻。単位 1/65536 us */
static uint64_t s_due_us;
static uint64_t s_ticks;
static uint32_t s_loops;
static uint32_t s_reslips;
static uint32_t s_cursor;  /* tick を中断したときの次のチャンネル */
static bool s_in_tick;

/* ---- 表 ---------------------------------------------------------------- */

/* 音程（1/64 半音 >> 6 = 音符番号 0-95）-> KC。OPM は下位 4bit で 3/7/11/15 を飛ばす。 */
static const uint8_t KEY_CODE[96] = {
    0x00, 0x01, 0x02, 0x04, 0x05, 0x06, 0x08, 0x09, 0x0a, 0x0c, 0x0d, 0x0e,
    0x10, 0x11, 0x12, 0x14, 0x15, 0x16, 0x18, 0x19, 0x1a, 0x1c, 0x1d, 0x1e,
    0x20, 0x21, 0x22, 0x24, 0x25, 0x26, 0x28, 0x29, 0x2a, 0x2c, 0x2d, 0x2e,
    0x30, 0x31, 0x32, 0x34, 0x35, 0x36, 0x38, 0x39, 0x3a, 0x3c, 0x3d, 0x3e,
    0x40, 0x41, 0x42, 0x44, 0x45, 0x46, 0x48, 0x49, 0x4a, 0x4c, 0x4d, 0x4e,
    0x50, 0x51, 0x52, 0x54, 0x55, 0x56, 0x58, 0x59, 0x5a, 0x5c, 0x5d, 0x5e,
    0x60, 0x61, 0x62, 0x64, 0x65, 0x66, 0x68, 0x69, 0x6a, 0x6c, 0x6d, 0x6e,
    0x70, 0x71, 0x72, 0x74, 0x75, 0x76, 0x78, 0x79, 0x7a, 0x7c, 0x7d, 0x7e,
};

/* CON -> キャリアのスロットマスク（bit0=M1 / bit1=M2 / bit2=C1 / bit3=C2） */
static const uint8_t CARRIER_SLOT[8] = {0x08, 0x08, 0x08, 0x08, 0x0c, 0x0e, 0x0e, 0x0f};

/* v0-v15 -> TL */
static const uint8_t VOLUME_TBL[16] = {
    0x2a, 0x28, 0x25, 0x22, 0x20, 0x1d, 0x1a, 0x18,
    0x15, 0x12, 0x10, 0x0d, 0x0a, 0x08, 0x05, 0x02,
};

/* TL 相当の 0-42 -> PCM8 の音量 0-15（参照実装 L000C6E） */
static const uint8_t PCM_VOLUME_TBL[43] = {
    15, 15, 15, 14, 14, 14, 13, 13, 13, 12, 12, 11, 11, 11, 10, 10,
    10,  9,  9,  8,  8,  8,  7,  7,  7,  6,  6,  5,  5,  5,  4,  4,
     4,  3,  3,  2,  2,  2,  1,  1,  1,  0,  0,
};

/* ---- 小物 -------------------------------------------------------------- */

/* MDX のオフセット・ワードはビッグエンディアン（68000） */
static uint16_t rd16(uint32_t at) {
    return (uint16_t)(((uint16_t)s_file[at] << 8) | s_file[at + 1u]);
}

static bool is_adpcm(const mdx_ch_t *c) {
    return (c->ch & 0x80u) != 0u;
}

/* OPM への書き込み。参照実装の L_WRITEOPM と同じくシャドウを更新する。 */
static void opm_put(uint8_t addr, uint8_t data) {
    opm_write(addr, data);
    s_opm[addr] = data;
    if (addr == 0x1bu) {
        s_opm_1b = data;
    }
}

/* 再生中に見つかった異常は非同期通知（vgm_fail() と同じ流儀） */
static void mdx_fail(const char *why, uint32_t at) {
    printf("# ERR mdx bad file (%s at 0x%08x)\n", why, (unsigned)at);
    s_state = MDX_STATE_ERROR;
}

/* 参照実装 L00117a の擬似乱数（LFO の波形 4 が使う） */
static uint16_t s_rnd_seed = 0x1234u;
static uint8_t rnd8(void) {
    s_rnd_seed = (uint16_t)(s_rnd_seed * 0xc549u + 0x0cu);
    return (uint8_t)(s_rnd_seed >> 8);
}

/* ---- ヘッダの解析 ------------------------------------------------------ */

/*
 * 構造（参照実装の L0007c0 が読む形）:
 *   タイトル (Shift_JIS)  0x0D 0x0A 0x1A で終端
 *   PDX ファイル名        0x00 で終端
 *   base: +0      Word  音色データのオフセット（base 相対）
 *         +2      Word * N  各チャンネルの MML のオフセット（base 相対）
 */
static const char *parse_header(void) {
    uint32_t at = 0;
    bool found = false;
    while (at + 3u <= s_size) {
        if (s_file[at] == 0x0Du && s_file[at + 1u] == 0x0Au && s_file[at + 2u] == 0x1Au) {
            found = true;
            break;
        }
        at++;
    }
    if (!found) {
        return "bad file";
    }

    uint32_t tlen = (at < sizeof(s_title) - 1u) ? at : (uint32_t)(sizeof(s_title) - 1u);
    memcpy(s_title, s_file, tlen);
    s_title[tlen] = '\0';

    uint32_t pat = at + 3u;
    uint32_t pend = pat;
    while (pend < s_size && s_file[pend] != 0x00u) {
        pend++;
    }
    if (pend >= s_size) {
        return "bad file";
    }
    uint32_t plen = pend - pat;
    if (plen > sizeof(s_pdx) - 1u) {
        plen = (uint32_t)(sizeof(s_pdx) - 1u);
    }
    memcpy(s_pdx, &s_file[pat], plen);
    s_pdx[plen] = '\0';

    s_base = pend + 1u;
    if (s_base + 4u > s_size) {
        return "bad file";
    }

    /*
     * チャンネル数は「先頭チャンネルのオフセットまでが表の大きさ」から出す。
     * 表は base から 2 + 2*N バイトなので N = (mml_off[0] - 2) / 2。
     * 9 = FM 8 + ADPCM 1、16 = FM 8 + ADPCM 8 (PCM8 拡張)。
     */
    uint16_t first = rd16(s_base + 2u);
    if (first < 4u || (first & 1u) != 0u) {
        return "bad file";
    }
    uint32_t nch = ((uint32_t)first - 2u) / 2u;
    if (nch != 9u && nch != 16u) {
        return "bad file";
    }
    s_nch = nch;

    if (s_base + 2u + 2u * nch > s_size) {
        return "bad file";
    }

    uint16_t voice_off = rd16(s_base);
    s_voice_top = s_base + voice_off;
    if (voice_off < first || s_voice_top > s_size) {
        return "bad file";
    }

    for (uint32_t i = 0; i < nch; i++) {
        uint32_t off = s_base + rd16(s_base + 2u + 2u * i);
        if (off >= s_size) {
            return "bad file";
        }
        s_mml_top[i] = off;
    }

    /*
     * 音色の索引。27 バイト固定長で先頭バイトが音色番号。
     * 参照実装は線形探索して最初の一致を採るので、索引も最初の一致を残す。
     */
    memset(s_voice, 0, sizeof(s_voice));
    for (uint32_t v = s_voice_top; v + 27u <= s_size; v += 27u) {
        uint8_t no = s_file[v];
        if (s_voice[no] == 0u) {
            s_voice[no] = (uint16_t)(v + 1u);
        }
    }

    return NULL;
}

static uint32_t voice_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < 256u; i++) {
        if (s_voice[i] != 0u) {
            n++;
        }
    }
    return n;
}

/* ---- 出力バックエンド -------------------------------------------------- */

/*
 * 音を出す操作はすべてここを通す。ADPCM チャンネルでは状態だけ更新して
 * 何も鳴らさない。将来 PCM8 を足すときはこの 6 本に手を入れる。
 */

/* 音色をレジスタへ展開する（参照実装 L000d84） */
static void snd_voice(mdx_ch_t *c) {
    if ((c->f17 & F17_VOICE_REQ) == 0u) {
        return;
    }
    c->f17 &= (uint8_t)~F17_VOICE_REQ;

    if (c->voice == 0u) {
        return; /* 未定義の音色番号。参照実装はダミーを使うが、ここでは触らない */
    }
    if (is_adpcm(c)) {
        return;
    }

    uint32_t a = c->voice;

    /* FL/CON は pan の下位 6bit に同居する */
    uint8_t flcon = s_file[a++];
    c->pan = (uint8_t)((c->pan & 0xc0u) | flcon);
    uint8_t carrier = CARRIER_SLOT[flcon & 0x07u];
    c->carrier = carrier;

    /* キーオンのスロットマスク */
    c->keyon_slot = (uint8_t)((s_file[a++] << 3) | c->ch);

    uint8_t reg = (uint8_t)(0x40u + c->ch);

    /* DT1/MUL */
    for (uint32_t i = 0; i < 4u; i++) {
        opm_put(reg, s_file[a++]);
        reg = (uint8_t)(reg + 8u);
    }
    /* TL。キャリアはいったん最小音量にして、直後の音量計算で入れ直す。 */
    uint8_t m = carrier;
    for (uint32_t i = 0; i < 4u; i++) {
        uint8_t d = s_file[a++];
        if ((m & 1u) != 0u) {
            d = 0x7fu;
        }
        m >>= 1;
        opm_put(reg, d);
        reg = (uint8_t)(reg + 8u);
    }
    /* KS/AR, AME/D1R, DT2/D2R, D1L/RR */
    for (uint32_t i = 0; i < 16u; i++) {
        opm_put(reg, s_file[a++]);
        reg = (uint8_t)(reg + 8u);
    }

    c->vol_last = 0xffu;
    c->f17 |= 0x64u; /* パン更新 + 参照実装が立てる 2 ビット */
}

/* パンと FL/CON（参照実装 L000e66） */
static void snd_pan(mdx_ch_t *c) {
    if ((c->f17 & F17_PAN_DIRTY) == 0u) {
        return;
    }
    c->f17 &= (uint8_t)~F17_PAN_DIRTY;
    if (is_adpcm(c)) {
        return;
    }
    opm_put((uint8_t)(0x20u + c->ch), c->pan);
}

/* キャリアの TL を書く（参照実装 L000e28） */
static void snd_tl(mdx_ch_t *c, uint8_t add) {
    c->vol_last = add;
    if (c->voice == 0u || is_adpcm(c)) {
        return;
    }
    uint32_t a = c->voice + 6u; /* FL/CON + スロットマスク + DT1/MUL 4 個を飛ばす */
    uint8_t m = c->carrier;
    uint8_t reg = (uint8_t)(0x60u + c->ch);
    for (uint32_t i = 0; i < 4u; i++) {
        uint8_t d = s_file[a++];
        if ((m & 1u) != 0u) {
            uint32_t t = (uint32_t)d + (uint32_t)add;
            d = (t < 0x80u) ? (uint8_t)t : 0x7fu;
            opm_put(reg, d);
        }
        m >>= 1;
        reg = (uint8_t)(reg + 8u);
    }
}

/* 音量（参照実装 L000dfe） */
static void snd_volume(mdx_ch_t *c) {
    uint32_t v = c->vol;
    if ((v & 0x80u) != 0u) {
        v &= 0x7fu; /* @v。TL 直接指定 */
    } else {
        v = VOLUME_TBL[v & 0x0fu];
    }

    v += s_fade_off;
    if (v >= 0x80u) {
        v = 0x7fu; /* 桁溢れは無音扱い */
    }
    v += (uint32_t)(c->vlfo.offset >> 8);
    if (v >= 0x80u) {
        v = 0x7fu;
    }

    if (c->vol_last == (uint8_t)v) {
        return;
    }
    snd_tl(c, (uint8_t)v);
}

/*
 * 音程（1/64 半音）を KC (0x28+ch) と KF (0x30+ch) の値へ落とす。
 * 参照実装 L000cdc と同じく 0x17FF で頭打ちにし、4 倍して上位バイトを
 * 音符番号、下位バイトを KF レジスタの値として使う。
 */
static void pitch_to_kc_kf(uint16_t pitch, uint8_t *kc, uint8_t *kf) {
    uint16_t v = pitch;
    if (v > 0x17ffu) {
        /* 上に溢れたら最高音、下に回り込んでいたら最低音 */
        v = ((int16_t)v >= 0) ? 0x17ffu : 0u;
    }
    v = (uint16_t)(v * 4u);
    *kf = (uint8_t)(v & 0xffu);
    *kc = KEY_CODE[v >> 8];
}

/* Timer-B 値と φM から 1 clock の長さ [us] を出す */
static uint32_t tick_us_for(uint8_t tempo, uint32_t phim_hz) {
    uint32_t period_cycles = 1024u * (256u - (uint32_t)tempo);
    return (uint32_t)(((uint64_t)period_cycles * 1000000ull) / (uint64_t)phim_hz);
}

/* 音程を KC / KF へ落とす（参照実装 L000cdc） */
static void snd_pitch(mdx_ch_t *c) {
    uint16_t d = (uint16_t)(c->pitch + (uint16_t)(c->bend >> 16) +
                            (uint16_t)(c->plfo.offset >> 16));
    if (d == c->pitch_out) {
        return;
    }
    c->pitch_out = d;

    if (is_adpcm(c)) {
        return;
    }

    uint8_t kc, kf;
    pitch_to_kc_kf(d, &kc, &kf);
    opm_put((uint8_t)(0x30u + c->ch), kf);
    opm_put((uint8_t)(0x28u + c->ch), kc);
}

/*
 * ADPCM チャンネルの音量を PCM8 の 0-15 へ落とす（参照実装 L000BE4 の前半）。
 * 範囲を外れたときは音量 0 を返し、呼び出し側は定位 0（= 停止）で発行する。
 */
static uint32_t pcm_volume(const mdx_ch_t *c, bool *audible) {
    uint32_t v = c->vol;
    if ((v & 0x80u) != 0u) {
        v &= 0x7fu; /* @v。TL 直接指定 */
    } else {
        v = VOLUME_TBL[v & 0x0fu];
    }

    /* 参照実装は add.b + bmi なので 8bit で丸めてから符号を見る */
    uint8_t t = (uint8_t)(v + s_fade_off);
    if ((t & 0x80u) != 0u || t >= 0x2bu) {
        *audible = false;
        return 0u;
    }
    *audible = true;
    return PCM_VOLUME_TBL[t];
}

/*
 * ADPCM チャンネルの定位（参照実装 L000B4E）。
 * 0xFC の側で 0 と 3 を入れ替えて格納してあり、ここでもう一度入れ替えるので
 * MML の p0 = 停止 / p3 = 左右に戻る。初期値 0 は左右になる。
 */
static int pcm_pan(const mdx_ch_t *c) {
    uint8_t p = (uint8_t)(c->pan & 0x03u);
    if (p == 0x00u || p == 0x03u) {
        p ^= 0x03u;
    }
    return (int)p;
}

/* キーオン（参照実装 L000e7e / ADPCM は L000BE4） */
static void snd_key_on(mdx_ch_t *c) {
    if (is_adpcm(c)) {
        bool audible = false;
        uint32_t vol = pcm_volume(c, &audible);
        int pan = audible ? pcm_pan(c) : 0; /* 定位 0 は PCM8 では停止 */
        uint32_t ch = (uint32_t)(c->ch & 0x07u);

        /* 参照実装は TRAP #2 を 2 回発行する（まず停止、次に発音） */
        pcm8_stop(ch);
        pcm8_key_on(ch, c->pcm_bank, (uint32_t)(c->pitch >> 6),
                    (int)((c->pan >> 2) & 0x1fu), (int)vol, pan);
        return;
    }
    opm_put(0x08u, c->keyon_slot);
}

/* キーオフ（参照実装 L000ff6 / ADPCM は L000CC6 = データ長 0 のチャネル停止） */
static void snd_key_off(mdx_ch_t *c) {
    if (is_adpcm(c)) {
        pcm8_stop((uint32_t)(c->ch & 0x07u));
        return;
    }
    opm_put(0x08u, c->ch);
}

/* ---- LFO --------------------------------------------------------------- */

/* 参照実装 L001562 相当。ピッチ LFO を頭から掛け直す。 */
static void plfo_reload(mdx_ch_t *c) {
    c->plfo.len_cnt = c->plfo.len_cooked;
    c->plfo.delta = c->plfo.delta_start;
    c->plfo.offset = c->plfo.offset_start;
}

/* 参照実装 L0015d0 相当 */
static void vlfo_reload(mdx_ch_t *c) {
    c->vlfo.len_cnt = c->vlfo.len;
    c->vlfo.delta = c->vlfo.delta_start;
    c->vlfo.offset = c->vlfo.delta_cooked;
}

/* 1 clock 分進める（参照実装 L0010b4 の 4 波形） */
static void plfo_step(mdx_ch_t *c) {
    uint32_t d = c->plfo.delta;
    switch (c->plfo.wave) {
    case 1:
        c->plfo.offset += d;
        if (--c->plfo.len_cnt == 0u) {
            c->plfo.len_cnt = c->plfo.len;
            c->plfo.offset = (uint32_t)(-(int32_t)c->plfo.offset);
        }
        break;
    case 2:
        c->plfo.offset = d;
        if (--c->plfo.len_cnt == 0u) {
            c->plfo.len_cnt = c->plfo.len;
            c->plfo.delta = (uint32_t)(-(int32_t)c->plfo.delta);
        }
        break;
    case 3:
        c->plfo.offset += d;
        if (--c->plfo.len_cnt == 0u) {
            c->plfo.len_cnt = c->plfo.len;
            c->plfo.delta = (uint32_t)(-(int32_t)c->plfo.delta);
        }
        break;
    case 4:
        if (--c->plfo.len_cnt == 0u) {
            int16_t r = (int16_t)(uint16_t)rnd8();
            c->plfo.offset = (uint32_t)((int32_t)r * (int32_t)(int16_t)(uint16_t)d);
            c->plfo.len_cnt = c->plfo.len;
        }
        break;
    default:
        break;
    }
}

/* 参照実装 L001116 の 4 波形 */
static void vlfo_step(mdx_ch_t *c) {
    uint16_t d = c->vlfo.delta;
    switch (c->vlfo.wave) {
    case 1:
        c->vlfo.offset = (uint16_t)(c->vlfo.offset + d);
        if (--c->vlfo.len_cnt == 0u) {
            c->vlfo.len_cnt = c->vlfo.len;
            c->vlfo.offset = c->vlfo.delta_cooked;
        }
        break;
    case 2:
        if (--c->vlfo.len_cnt == 0u) {
            c->vlfo.len_cnt = c->vlfo.len;
            c->vlfo.offset = (uint16_t)(c->vlfo.offset + d);
            c->vlfo.delta = (uint16_t)(-(int16_t)c->vlfo.delta);
        }
        break;
    case 3:
        c->vlfo.offset = (uint16_t)(c->vlfo.offset + d);
        if (--c->vlfo.len_cnt == 0u) {
            c->vlfo.len_cnt = c->vlfo.len;
            c->vlfo.delta = (uint16_t)(-(int16_t)c->vlfo.delta);
        }
        break;
    case 4:
        if (--c->vlfo.len_cnt == 0u) {
            int16_t r = (int16_t)(uint16_t)rnd8();
            c->vlfo.offset = (uint16_t)((int32_t)(int16_t)d * (int32_t)r);
            c->vlfo.len_cnt = c->vlfo.len;
        }
        break;
    default:
        break;
    }
}

/* LFO ディレイの消化（参照実装 L001094） */
static void lfo_delay_step(mdx_ch_t *c) {
    if (--c->lfo_cnt != 0u) {
        return;
    }
    if ((c->f16 & F16_PLFO) != 0u) {
        plfo_reload(c);
    }
    if ((c->f16 & F16_VLFO) != 0u) {
        vlfo_reload(c);
    }
}

/* ---- MML の実行 -------------------------------------------------------- */

static void ch_fetch(mdx_ch_t *c, uint32_t idx);

/* 音符・休符の長さを確定させる（参照実装 L001216） */
static void set_len(mdx_ch_t *c, uint32_t gate, uint32_t len, uint32_t p) {
    c->gate = (uint8_t)(gate + 1u);
    c->len = (uint8_t)(len + 1u);
    c->p = p;
}

/*
 * 0xE1-0xFF のコマンドを 1 つ実行する。
 * 続けて MML を読むなら false、この tick の読み出しを終えるなら true。
 */
/*
 * 演奏終了（参照実装 L001442）。参照実装はチャンネルのポインタを静的な
 * { 0x7f, 0xf1, 0x00 } へ向け直して「128 clock 休んでまた終わる」を繰り返させる。
 * ここでは同じ効果を、いま読んだ 0xF1 / 0xE7 の位置へ戻すことで作る。
 * こうしないと、この先にある別チャンネルの MML や音色データを読んでしまう。
 */
static void end_channel(mdx_ch_t *c, uint32_t idx, uint32_t op_at) {
    s_enable &= (uint16_t)~(1u << idx);
    set_len(c, 0x7fu, 0x7fu, op_at);
}

static bool cmd_exec(mdx_ch_t *c, uint32_t idx, uint8_t op, uint32_t *pp) {
    uint32_t p = *pp;
    uint32_t op_at = p - 1u; /* いま読んだオペコードの位置 */

    switch (op) {
    case 0xFFu: { /* @t テンポ */
        uint8_t t = s_file[p++];
        s_tempo = t;
        opm_put(0x12u, t);
        break;
    }
    case 0xFEu: { /* OPM レジスタ直書き */
        uint8_t r = s_file[p++];
        uint8_t d = s_file[p++];
        if (r == 0x12u) {
            s_tempo = d; /* テンポもこちらで変えられる */
        }
        opm_put(r, d);
        break;
    }
    case 0xFDu: { /* @ 音色。実際のレジスタ書き込みはキーオン時まで遅延する。 */
        uint8_t no = s_file[p++];
        if (is_adpcm(c)) {
            c->pcm_bank = no;
        } else {
            c->voice = s_voice[no];
            c->f17 |= F17_VOICE_REQ;
        }
        break;
    }
    case 0xFCu: { /* p パン */
        uint8_t v = s_file[p++];
        if (is_adpcm(c)) {
            /* 参照実装は 0 と 3 を入れ替える */
            if (v == 0x00u || v == 0x03u) {
                v ^= 0x03u;
            }
            c->pan = (uint8_t)((c->pan & 0xfcu) | (v & 0x03u));
        } else {
            c->pan = (uint8_t)((c->pan & 0x3fu) | (uint8_t)(v << 6));
            c->f17 |= F17_PAN_DIRTY;
        }
        break;
    }
    case 0xFBu: /* v 音量 */
        c->vol = s_file[p++];
        c->f17 |= F17_VOL_DIRTY;
        if (is_adpcm(c) && s_tie_flag) {
            /*
             * 参照実装 PCM_Vol。タイでつながっている間だけ、発音中の
             * ADPCM の音量を差し替える（周波数と定位は据え置き）。
             */
            bool audible = false;
            uint32_t vol = pcm_volume(c, &audible);
            pcm8_set_mode((uint32_t)(c->ch & 0x07u), (int)vol, -1, audible ? -1 : 0);
        }
        break;
    case 0xFAu: /* v- */
        if ((c->vol & 0x80u) == 0u) {
            if (c->vol != 0u) {
                c->vol--;
                c->f17 |= F17_VOL_DIRTY;
            }
        } else if (c->vol != 0xffu) {
            c->vol++; /* @v は値が大きいほど TL が大きい = 小さい音 */
            c->f17 |= F17_VOL_DIRTY;
        }
        break;
    case 0xF9u: /* v+ */
        if ((c->vol & 0x80u) == 0u) {
            if (c->vol != 0x0fu) {
                c->vol++;
                c->f17 |= F17_VOL_DIRTY;
            }
        } else if (c->vol != 0x80u) {
            c->vol--;
            c->f17 |= F17_VOL_DIRTY;
        }
        break;
    case 0xF8u: /* q ゲートタイム */
        c->q = s_file[p++];
        break;
    case 0xF7u: /* & レガート */
        c->f16 |= F16_LEGATO;
        break;
    case 0xF6u: { /* リピート開始。回数を次のバイト（カウンタ）へ書き込む。 */
        uint8_t n = s_file[p];
        s_file[p + 1u] = n;
        p += 2u;
        break;
    }
    case 0xF5u: { /* リピート終了 */
        uint16_t w = rd16(p);
        p += 2u;
        uint32_t back = (uint32_t)((uint16_t)(~w + 1u)); /* 2 の補数で戻る距離 */
        if (back == 0u || back + 1u > p) {
            mdx_fail("repeat", p);
            return true;
        }
        uint32_t cnt_at = p - back - 1u;
        s_file[cnt_at] = (uint8_t)(s_file[cnt_at] - 1u);
        if (s_file[cnt_at] != 0u) {
            p -= back;
        }
        break;
    }
    case 0xF4u: { /* リピート脱出。最終回だけ飛ぶ。 */
        uint16_t w = rd16(p);
        p += 2u;
        uint32_t to = p + w;
        if (to + 2u > s_size) {
            mdx_fail("repeat escape", p);
            return true;
        }
        uint16_t w2 = rd16(to);
        uint32_t back = (uint32_t)((uint16_t)(~w2 + 1u));
        uint32_t at = to + 2u;
        if (back == 0u || back + 1u > at) {
            mdx_fail("repeat escape", p);
            return true;
        }
        if (s_file[at - back - 1u] == 0x01u) {
            p = at;
        }
        break;
    }
    case 0xF3u: /* D ディチューン */
        c->detune = rd16(p);
        p += 2u;
        break;
    case 0xF2u: { /* ポルタメント */
        int32_t v = (int32_t)(int16_t)rd16(p);
        p += 2u;
        c->bend_delta = (uint32_t)(v << 8);
        c->f16 |= F16_PORTA;
        break;
    }
    case 0xF1u: { /* 演奏終了 / ループ */
        if (s_file[p] == 0x00u) {
            end_channel(c, idx, op_at);
            return true;
        }
        uint16_t w = rd16(p);
        p += 2u;
        uint32_t back = (uint32_t)((uint16_t)(~w + 1u));
        if (back == 0u || back > p) {
            mdx_fail("loop", p);
            return true;
        }
        p -= back;
        s_loops++;
        break;
    }
    case 0xF0u: /* キーオン遅延 */
        c->keyon_delay = s_file[p++];
        break;
    case 0xEFu: { /* 同期送出 */
        uint8_t n = s_file[p++];
        if (n < MDX_CH_MAX) {
            s_sync[n] = 0xffu;
        }
        break;
    }
    case 0xEEu: /* 同期待ち */
        c->f17 |= F17_SYNC_WAIT;
        c->p = p;
        return true;
    case 0xEDu: { /* ADPCM のサンプリング周波数 / FM のノイズ周波数 */
        uint8_t v = s_file[p++];
        if (is_adpcm(c)) {
            c->pan = (uint8_t)((c->pan & 0x03u) | (uint8_t)(v << 2));
        } else {
            opm_put(0x0fu, v);
        }
        break;
    }
    case 0xECu: { /* ピッチ LFO */
        c->f16 |= F16_PLFO;
        uint8_t b = s_file[p++];
        if ((b & 0x80u) != 0u) {
            if ((b & 0x01u) != 0u) {
                plfo_reload(c);
            } else {
                c->f16 &= (uint8_t)~F16_PLFO;
                c->plfo.offset = 0;
            }
            break;
        }
        uint8_t sel = (uint8_t)(b & 0x03u);
        c->plfo.wave = (uint8_t)(sel + 1u);
        uint16_t len = rd16(p);
        p += 2u;
        c->plfo.len = len;
        uint16_t cooked = len;
        if (sel != 1u) {
            cooked = (uint16_t)(cooked >> 1);
            if (sel == 3u) {
                cooked = 1u;
            }
        }
        c->plfo.len_cooked = cooked;
        int32_t d = (int32_t)(int16_t)rd16(p);
        p += 2u;
        d <<= 8;
        if (b >= 0x04u) {
            d <<= 8;
        }
        c->plfo.delta_start = (uint32_t)d;
        c->plfo.offset_start = (sel == 2u) ? (uint32_t)d : 0u;
        plfo_reload(c);
        break;
    }
    case 0xEBu: { /* 音量 LFO */
        c->f16 |= F16_VLFO;
        uint8_t b = s_file[p++];
        if ((b & 0x80u) != 0u) {
            if ((b & 0x01u) != 0u) {
                vlfo_reload(c);
            } else {
                c->f16 &= (uint8_t)~F16_VLFO;
                c->vlfo.offset = 0;
            }
            break;
        }
        c->vlfo.wave = (uint8_t)(b + 1u);
        uint16_t len = rd16(p);
        p += 2u;
        c->vlfo.len = len;
        uint16_t dv = rd16(p);
        p += 2u;
        c->vlfo.delta_start = dv;
        int32_t t;
        if ((b & 0x01u) != 0u) {
            t = (int32_t)(int16_t)dv;
        } else {
            t = (int32_t)(int16_t)dv * (int32_t)(int16_t)len;
        }
        t = -(int16_t)t;
        if ((int16_t)t >= 0) {
            t = 0;
        }
        c->vlfo.delta_cooked = (uint16_t)t;
        vlfo_reload(c);
        break;
    }
    case 0xEAu: { /* OPM のハードウェア LFO */
        uint8_t b = s_file[p++];
        if ((b & 0x80u) != 0u) {
            /* bit0 が 0 ならこのチャンネルの LFO を切り、1 なら元の深さへ戻す */
            opm_put((uint8_t)(0x38u + c->ch), ((b & 0x01u) != 0u) ? c->pms_ams : 0x00u);
            break;
        }
        c->f16 &= (uint8_t)~F16_LFO_SYNC;
        if ((b & 0x40u) != 0u) {
            c->f16 |= F16_LFO_SYNC;
            b = (uint8_t)(b & ~0x40u);
        }
        /* 0x1B は上位 2bit が CT1/CT2 なので残す（参照実装が控えている理由） */
        opm_put(0x1bu, (uint8_t)(b | (s_opm_1b & 0xc0u)));
        opm_put(0x18u, s_file[p++]); /* LFRQ */
        opm_put(0x19u, s_file[p++]); /* PMD / AMD */
        opm_put(0x19u, s_file[p++]);
        c->pms_ams = s_file[p++];
        opm_put((uint8_t)(0x38u + c->ch), c->pms_ams);
        break;
    }
    case 0xE9u: /* LFO ディレイ */
        c->lfo_delay = s_file[p++];
        break;
    case 0xE8u: /* PCM8 モード宣言 */
        s_pcm8_mode = true;
        s_enable |= 0xfe00u;
        break;
    case 0xE7u: { /* 拡張 */
        uint8_t sub = s_file[p++];
        switch (sub) {
        case 0x01u: /* フェードアウト */
            s_fade_speed = s_file[p++];
            s_fade_on = 0xffu;
            break;
        case 0x02u: { /* PCM8 へのコマンド。d0.w + d1.l がビッグエンディアンで並ぶ。 */
            uint16_t fn = rd16(p);
            uint32_t d1 = ((uint32_t)s_file[p + 2u] << 24) |
                          ((uint32_t)s_file[p + 3u] << 16) |
                          ((uint32_t)s_file[p + 4u] << 8) | (uint32_t)s_file[p + 5u];
            p += 6u;
            if ((fn & 0xfff0u) == 0x0070u) { /* 動作モード変更 */
                int vol = (int)(int8_t)(uint8_t)(d1 >> 16);
                int mode = (int)(int8_t)(uint8_t)(d1 >> 8);
                int pan = (int)(int8_t)(uint8_t)d1;
                pcm8_set_mode((uint32_t)(fn & 0x07u), vol, mode, pan);
            } else if (fn == 0x0100u) { /* 終了。鳴っている分は鳴らし切る。 */
                pcm8_end_all();
            } else if (fn == 0x0101u) { /* 一時停止 = 即時打ち切り */
                pcm8_abort_all();
            }
            /* $01FC（多重/単音モード）などは本機に効く設定が無いので無視する */
            break;
        }
        case 0x03u: /* キーオフ抑止 */
            if (s_file[p++] != 0u) {
                c->f16 |= F16_NO_KEYOFF;
            } else {
                c->f16 &= (uint8_t)~F16_NO_KEYOFF;
            }
            break;
        case 0x05u: { /* 休符を置いて即キーオン */
            uint8_t n = s_file[p++];
            set_len(c, n, n, p);
            c->f16 &= (uint8_t)~F16_KEYON_REQ;
            c->f16 |= F16_KEYED;
            snd_key_on(c);
            return true;
        }
        case 0x06u: /* 参照実装ではフラグの上げ下げのみ。FM の発音には効かない。 */
            p++;
            break;
        default: /* 0x00 と 0x04 以上は演奏終了扱い */
            end_channel(c, idx, op_at);
            return true;
        }
        break;
    }
    default: /* 0xE1-0xE6 は参照実装でも演奏終了と同じハンドラ */
        end_channel(c, idx, op_at);
        return true;
    }

    *pp = p;
    return false;
}

/* 次の音符・休符まで MML を読む（参照実装 L0011d4） */
static void ch_fetch(mdx_ch_t *c, uint32_t idx) {
    uint32_t p = c->p;
    c->f16 &= 0x7bu; /* ポルタメントとレガートを落とす */

    for (uint32_t guard = 0; guard < 4096u; guard++) {
        if (p >= s_size) {
            mdx_fail("truncated", p);
            return;
        }
        uint8_t op = s_file[p++];

        if (op < 0x80u) { /* 休符 */
            set_len(c, op, op, p);
            return;
        }
        if (op >= 0xE0u) { /* コマンド */
            if (cmd_exec(c, idx, op, &p)) {
                return;
            }
            if (s_state != MDX_STATE_PLAYING) {
                return;
            }
            continue;
        }

        /* 音符 */
        if (p >= s_size) {
            mdx_fail("truncated", p);
            return;
        }
        uint32_t pitch = (uint32_t)(op & 0x7fu) << 6; /* 1/64 半音 */
        pitch += 5u;                                  /* 参照実装の調律ぶん */
        pitch += c->detune;
        c->pitch = (uint16_t)pitch;
        c->f16 |= F16_KEYON_REQ;
        c->keyon_cnt = c->keyon_delay;

        uint32_t l = s_file[p++];
        uint32_t g;
        if ((c->q & 0x80u) != 0u) {
            /* @q は負のオフセット。バイトの桁が上がらなければ 0 まで詰める。 */
            uint32_t t = (uint32_t)c->q + l;
            g = (t >= 0x100u) ? (t & 0xffu) : 0u;
        } else {
            g = ((uint32_t)c->q * l) >> 3;
        }
        set_len(c, g, l, p);
        return;
    }

    mdx_fail("no note", c->p);
}

/* ---- 1 clock 分の処理 -------------------------------------------------- */

/* ポルタメントと LFO（参照実装 L001050） */
static void ch_pre(mdx_ch_t *c) {
    if (is_adpcm(c)) {
        return;
    }
    if ((c->f16 & F16_PORTA) != 0u && c->keyon_cnt == 0u) {
        c->bend += c->bend_delta;
    }
    if (c->lfo_delay != 0u) {
        if (c->keyon_cnt != 0u) {
            return;
        }
        if (c->lfo_cnt != 0u) {
            lfo_delay_step(c);
            return;
        }
    }
    if ((c->f16 & F16_PLFO) != 0u) {
        plfo_step(c);
    }
    if ((c->f16 & F16_VLFO) != 0u) {
        vlfo_step(c);
    }
}

/* キーオフ（参照実装 L000fe6） */
static void ch_key_off(mdx_ch_t *c) {
    bool was = (c->f16 & F16_KEYED) != 0u;
    c->f16 &= (uint8_t)~F16_KEYED;
    if (!was) {
        return;
    }
    if ((c->f16 & F16_NO_KEYOFF) != 0u) {
        return;
    }
    snd_key_off(c);
}

/* 長さの消化と次の読み出し（参照実装 L0011b4 + L0011ce） */
static void ch_len(mdx_ch_t *c, uint32_t idx) {
    /* 参照実装 L000EA2 の sne.b TieFlag。次の読み出しへ渡る前に写しておく。 */
    s_tie_flag = (c->f16 & F16_LEGATO) != 0u;

    if ((c->f17 & F17_SYNC_WAIT) != 0u) {
        /* 同期待ち。信号が来ていたら降ろして読み進める。 */
        if (s_sync[idx] == 0u) {
            return;
        }
        s_sync[idx] = 0u;
        c->f17 &= (uint8_t)~F17_SYNC_WAIT;
        ch_fetch(c, idx);
        return;
    }

    if ((c->f16 & F16_LEGATO) == 0u) {
        if (--c->gate == 0u) {
            ch_key_off(c);
        }
    }

    if (--c->len != 0u) {
        return;
    }
    ch_fetch(c, idx);
}

/* キーオンとレジスタへの反映（参照実装 L000c66） */
static void ch_post(mdx_ch_t *c) {
    if ((c->f16 & F16_KEYON_REQ) == 0u) {
        if (!is_adpcm(c)) {
            snd_pitch(c);
            snd_volume(c);
        }
        return;
    }

    if (c->keyon_cnt != 0u) {
        c->keyon_cnt--;
        if (!is_adpcm(c)) {
            snd_pitch(c);
            snd_volume(c);
        }
        return;
    }

    if (!is_adpcm(c)) {
        snd_voice(c);
        snd_pan(c);

        if ((c->f16 & F16_KEYED) == 0u) {
            c->lfo_cnt = c->lfo_delay;
            if (c->lfo_cnt != 0u) {
                c->plfo.offset = 0;
                c->vlfo.offset = 0;
                lfo_delay_step(c);
            }
            if ((c->f16 & F16_LFO_SYNC) != 0u) {
                /* OPM の LFO 位相をリセットする */
                opm_put(0x01u, 0x02u);
                opm_put(0x01u, 0x00u);
            }
        }

        c->bend = 0;
        snd_pitch(c);
        snd_volume(c);
    }

    /* キーオン（参照実装 L000e7e） */
    bool was = (c->f16 & F16_KEYED) != 0u;
    c->f16 |= F16_KEYED;
    if (!was) {
        /*
         * キーオフを抑止していると鳴りっぱなしなので、鳴らし直す直前に
         * 一度だけキーオフを入れる（参照実装 L000e7e）。
         */
        if ((c->f16 & F16_NO_KEYOFF) != 0u) {
            snd_key_off(c);
        }
        snd_key_on(c);
    }

    c->f16 &= (uint8_t)~F16_KEYON_REQ;
}

/* フェードアウト（参照実装 L0009be）。終わったら true。 */
static bool fade_step(void) {
    if (s_fade_on == 0u) {
        return false;
    }
    if ((int8_t)s_fade_on < 0) {
        s_fade_on = 0x7fu;
        s_fade_cnt = s_fade_speed;
    }
    if ((int16_t)s_fade_cnt >= 0) {
        s_fade_cnt = (uint16_t)(s_fade_cnt - 2u);
        return false;
    }
    if ((int8_t)s_fade_off >= 0x0a) {
        s_fade_flag = 0xffu;
    }
    if ((int8_t)s_fade_off >= 0x3e) {
        return true;
    }
    s_fade_off++;
    s_fade_cnt = s_fade_speed;
    return false;
}

/* ---- 停止 -------------------------------------------------------------- */

static void key_off_all(void) {
    /*
     * 全 8 チャンネルをキーオフする。8 回 x 32us = 約 256us。
     * opm_reset() は 20ms ブロックして I2S アンダーランが確定するので使わない。
     */
    for (uint8_t ch = 0; ch < 8u; ch++) {
        opm_put(0x08u, ch);
    }
    pcm8_abort_all();
}

/* ---- 一覧 -------------------------------------------------------------- */

static int name_cmp(const char *a, const char *b) {
    for (;;) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'a' && ca <= 'z') {
            ca = (unsigned char)(ca - 'a' + 'A');
        }
        if (cb >= 'a' && cb <= 'z') {
            cb = (unsigned char)(cb - 'a' + 'A');
        }
        if (ca != cb) {
            return (ca < cb) ? -1 : 1;
        }
        if (ca == '\0') {
            return 0;
        }
    }
}

static bool has_mdx_ext(const char *name) {
    size_t n = strlen(name);
    if (n < 4u) {
        return false;
    }
    return name_cmp(name + n - 4u, ".MDX") == 0;
}

static bool is_listable(const FILINFO *fi) {
    if ((fi->fattrib & (AM_DIR | AM_SYS | AM_HID)) != 0u) {
        return false;
    }
    return has_mdx_ext(fi->fname);
}

const char *mdx_list(void (*tick)(void)) {
    if (!storage_fatfs_may_access()) {
        return "wrong state";
    }
    if (storage_fs_state() != STORAGE_FS_MOUNTED) {
        return "no filesystem";
    }

    /*
     * vgm_list() と同じく、名前 2 個ぶんの RAM で「直前より大きいものの中で最小」を
     * 毎回ディレクトリ走査で探す。
     */
    char prev[FF_LFN_BUF + 1];
    char best[FF_LFN_BUF + 1];
    static FILINFO fi;
    DIR dir;

    prev[0] = '\0';
    bool first = true;

    for (;;) {
        FRESULT fr = f_opendir(&dir, MDX_DIR);
        if (fr == FR_NO_PATH || fr == FR_NO_FILE) {
            return "not found";
        }
        if (fr != FR_OK) {
            return "io error";
        }

        bool found = false;
        uint32_t best_size = 0;
        best[0] = '\0';

        for (;;) {
            if (f_readdir(&dir, &fi) != FR_OK || fi.fname[0] == '\0') {
                break;
            }
            if (!is_listable(&fi)) {
                continue;
            }
            if (!first && name_cmp(fi.fname, prev) <= 0) {
                continue;
            }
            if (!found || name_cmp(fi.fname, best) < 0) {
                snprintf(best, sizeof(best), "%s", fi.fname);
                best_size = (uint32_t)fi.fsize;
                found = true;
            }
        }

        f_closedir(&dir);

        if (!found) {
            break;
        }

        /* サイズを先に置く。名前は空白を含みうるので必ず最後の欄にする。 */
        printf("# file    : %9u %s\n", (unsigned)best_size, best);
        if (tick != NULL) {
            tick();
        }

        snprintf(prev, sizeof(prev), "%s", best);
        first = false;
    }

    return NULL;
}

/* ---- チャンネルの初期化 ------------------------------------------------ */

/*
 * 参照実装 L000866 の初期値をそのまま写す。
 *   FM   : pan = 0xC0（L+R）/ v = 8 / Q = 8 / len = 1 / 音程は無効
 *   ADPCM: pan = 0x10 / v = 8 / ch に bit7 と PCM ch 番号
 * FM は加えて PMS/AMS (0x38+ch) に 0 を書く。
 */
static void init_channels(void) {
    memset(s_ch, 0, sizeof(s_ch));
    memset(s_sync, 0, sizeof(s_sync));

    for (uint32_t i = 0; i < s_nch; i++) {
        mdx_ch_t *c = &s_ch[i];

        c->p = s_mml_top[i];
        c->pitch_out = 0xffffu;
        c->vol_last = 0xffu;
        c->len = 1u;
        c->gate = 1u;
        c->vol = 0x08u;
        c->q = 0x08u;

        if (i < MDX_FM_CH) {
            c->ch = (uint8_t)i;
            c->pan = 0xc0u;
            opm_put((uint8_t)(0x38u + i), 0x00u);
        } else {
            c->ch = (uint8_t)(0x80u | ((i - MDX_FM_CH) & 0x07u));
            c->pan = 0x10u;
        }
    }

    s_enable = 0x01ffu;
    s_pcm8_mode = false;
    s_fade_off = 0;
    s_fade_flag = 0;
    s_fade_on = 0;
    s_fade_speed = 0;
    s_fade_cnt = 0;
    s_rnd_seed = 0x1234u;
}

/* ---- 操作 -------------------------------------------------------------- */

void mdx_init(void) {
    s_state = MDX_STATE_STOPPED;
    s_name[0] = '\0';
    s_title[0] = '\0';
    s_pdx[0] = '\0';
    s_size = 0;
    s_nch = 0;
    s_ticks = 0;
    s_loops = 0;
    s_reslips = 0;
    s_cursor = 0;
    s_in_tick = false;
    s_tempo = 200u;
}

/*
 * ヘッダの PDX 名から /MDX/<name>.PDX を組み立てる。
 *
 * 名前はファイルの中身なので信用しない。ディレクトリを跨がせず、制御文字も弾く。
 * 拡張子は付いていないのが普通だが、付いていたら重ねない。
 * FatFs の LFN 照合は大小を無視するので、実体が小文字でも当たる。
 */
static bool pdx_path(char *out, size_t out_len) {
    char base[MDX_PDX_MAX];
    size_t n = 0;

    for (const char *q = s_pdx; *q != '\0' && n < sizeof(base) - 1u; q++) {
        unsigned char ch = (unsigned char)*q;
        if (ch < 0x20u || ch == '/' || ch == '\\' || ch == ':') {
            return false;
        }
        base[n++] = (char)ch;
    }
    while (n > 0u && base[n - 1u] == ' ') {
        n--; /* 末尾の空白は落とす */
    }
    base[n] = '\0';
    if (n == 0u || strcmp(base, ".") == 0 || strcmp(base, "..") == 0) {
        return false;
    }

    /* すでに .PDX が付いていたら重ねない */
    if (n > 4u) {
        const char *ext = &base[n - 4u];
        if ((ext[0] == '.') && (ext[1] == 'p' || ext[1] == 'P') &&
            (ext[2] == 'd' || ext[2] == 'D') && (ext[3] == 'x' || ext[3] == 'X')) {
            base[n - 4u] = '\0';
        }
    }

    int len = snprintf(out, out_len, "%s/%s.PDX", MDX_DIR, base);
    return len > 0 && (size_t)len < out_len;
}

/* PDX を開く。曲が PDX を要求していなければ何もしない。 */
static void open_pdx(void) {
    if (s_pdx[0] == '\0') {
        return;
    }
    printf("# pdx     : %s\n", s_pdx);

    char path[16 + MDX_PDX_MAX];
    if (!pdx_path(path, sizeof(path))) {
        printf("# hint    : PDX 名が不正。ADPCM パートは鳴らない\n");
        return;
    }

    const char *err = pcm8_open_pdx(path);
    if (err != NULL) {
        printf("# hint    : %s を開けない (%s)。ADPCM パートは鳴らない\n", path, err);
        return;
    }
    printf("# adpcm   : %s\n", path);
    if (!pcm8_enabled()) {
        /* mdx pcm off は起動まで残るので、鳴らない理由をここで必ず出す */
        printf("# hint    : ADPCM のミキシングは off。mdx pcm on で戻す\n");
    }
}

const char *mdx_play(const char *name) {
    if (!storage_fatfs_may_access()) {
        return "wrong state";
    }
    if (storage_fs_state() != STORAGE_FS_MOUNTED) {
        return "no filesystem";
    }
    if (s_state == MDX_STATE_PLAYING || vgm_is_playing()) {
        return "wrong state";
    }

    /* 名前の検査。ディレクトリを跨がせない。 */
    size_t len = strlen(name);
    if (len == 0u || len > sizeof(s_name) - 1u) {
        return "bad argument";
    }
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
        return "bad argument";
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return "bad argument";
    }

    char path[8 + sizeof(s_name)];
    snprintf(path, sizeof(path), "%s/%s", MDX_DIR, name);

    FIL fp;
    FRESULT fr = f_open(&fp, path, FA_READ);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
        return "not found";
    }
    if (fr != FR_OK) {
        return "io error";
    }

    FSIZE_t fsize = f_size(&fp);
    if (fsize == 0u || fsize > MDX_MAX_BYTES) {
        f_close(&fp);
        printf("# hint    : MDX は %u バイトまで\n", (unsigned)MDX_MAX_BYTES);
        return "bad file";
    }

    UINT got = 0;
    fr = f_read(&fp, s_file, (UINT)fsize, &got);
    f_close(&fp);
    if (fr != FR_OK || got != (UINT)fsize) {
        return "io error";
    }
    s_size = (uint32_t)got;

    const char *err = parse_header();
    if (err != NULL) {
        return err;
    }

    /*
     * φM を MDX の前提（X68000 = 4MHz）へ寄せる。clock fixed なら何もしない。
     * 時計を打つ前に済ませる（切り替えの数百 us を再生の起点に乗せない）。
     */
    err = clockmode_follow_file(4000000u);
    if (err != NULL) {
        return err;
    }
    uint32_t actual = opm_clock_hz_actual();
    if (actual != 4000000u) {
        printf("# clock   : mdx 4000000 Hz / phiM %u Hz (音程もテンポもずれる)\n",
               (unsigned)actual);
    }

    /* 前の曲の PDX が開いたままなら閉じる（ERROR のまま play できるため） */
    pcm8_close_pdx();

    /* レジスタを既知の状態にしてから始める。opm_reset() は 20ms 止まるので使わない。 */
    opm_clear();
    memset(s_opm, 0, sizeof(s_opm));
    s_opm_1b = 0;

    snprintf(s_name, sizeof(s_name), "%s", name);
    s_tempo = 200u;
    s_ticks = 0;
    s_loops = 0;
    s_reslips = 0;
    s_cursor = 0;
    s_in_tick = false;
    s_tie_flag = false;

    init_channels();

    printf("# mdx     : %s\n", s_name);
    printf("# title   : %s\n", s_title);
    printf("# ch      : %u  voices %u\n", (unsigned)s_nch, (unsigned)voice_count());
    open_pdx();

    uint64_t now = time_us_64();
    s_due_us = now;
    s_due_q = now << 16;
    s_state = MDX_STATE_PLAYING;

    return NULL;
}

const char *mdx_stop(void) {
    if (s_state != MDX_STATE_PLAYING && s_state != MDX_STATE_ERROR) {
        return "wrong state";
    }

    key_off_all();
    pcm8_close_pdx();
    s_state = MDX_STATE_STOPPED;
    s_cursor = 0;
    s_in_tick = false;

    return NULL;
}

/* ---- サービス ---------------------------------------------------------- */

/* 次の tick の予定時刻を進める。Timer-B の周期は φM サイクルでちょうど整数。 */
static void advance_tick(void) {
    uint32_t period_cycles = 1024u * (256u - (uint32_t)s_tempo);
    uint32_t phim = opm_clock_hz_actual();
    s_due_q += ((uint64_t)period_cycles * 65536ull * 1000000ull) / (uint64_t)phim;
    s_due_us = s_due_q >> 16;
    s_ticks++;
}

/* この tick で回すチャンネル数。9-15 は 0xE8 が出てから。 */
static uint32_t active_channels(void) {
    if (s_pcm8_mode && s_nch > 9u) {
        return s_nch;
    }
    return (s_nch < 9u) ? s_nch : 9u;
}

bool mdx_service(void) {
    if (s_state != MDX_STATE_PLAYING) {
        return false;
    }

    if (!s_in_tick) {
        uint64_t now = time_us_64();
        if (now < s_due_us) {
            return false;
        }

        uint32_t lag = (uint32_t)(now - s_due_us);
        stats_seq_lag_update(lag);
        if (lag > MDX_RESYNC_LAG_US) {
            /* 長く止まっていた。早送りせず時計を張り直す。 */
            s_due_us = now;
            s_due_q = now << 16;
            s_reslips++;
        }

        if (fade_step()) {
            key_off_all();
            pcm8_close_pdx();
            s_state = MDX_STATE_STOPPED;
            printf("# mdx     : fadeout end\n");
            return true;
        }

        /* 参照実装は毎 tick テンポを書き直す */
        opm_put(0x12u, s_tempo);

        s_in_tick = true;
        s_cursor = 0;
    }

    uint32_t nch = active_channels();
    uint32_t t0 = time_us_32();

    while (s_cursor < nch) {
        mdx_ch_t *c = &s_ch[s_cursor];
        uint32_t idx = s_cursor;

        ch_pre(c);
        ch_len(c, idx);
        if (s_state != MDX_STATE_PLAYING) {
            return true;
        }
        ch_post(c);

        s_cursor++;

        if (time_us_32() - t0 >= MDX_BUDGET_US) {
            return true; /* 続きは次の周回。tick はまだ進めない。 */
        }
    }

    /* 参照実装が tick の終わりに書くタイマ制御 */
    opm_put(0x14u, 0x1bu);

    s_in_tick = false;
    advance_tick();

    if (s_enable == 0u) {
        key_off_all();
        pcm8_close_pdx();
        s_state = MDX_STATE_STOPPED;
        printf("# mdx     : end\n");
    }

    return true;
}

/* ---- 問い合わせ -------------------------------------------------------- */

mdx_state_t mdx_state(void) {
    return s_state;
}

const char *mdx_state_name(void) {
    switch (s_state) {
    case MDX_STATE_PLAYING:
        return "PLAYING";
    case MDX_STATE_ERROR:
        return "ERROR";
    default:
        return "STOPPED";
    }
}

bool mdx_is_playing(void) {
    return s_state == MDX_STATE_PLAYING;
}

const char *mdx_current_name(void) {
    return s_name;
}

const char *mdx_title(void) {
    return s_title;
}

const char *mdx_pdx_name(void) {
    return s_pdx;
}

uint32_t mdx_channels(void) {
    return s_nch;
}

uint64_t mdx_tick_count(void) {
    return s_ticks;
}

/*
 * ループジャンプ (0xF1 nn) の回数。参照実装と同じく全チャンネルの合計で、
 * 「曲が何周したか」ではない。短いパターンを回すチャンネルがあると速く増える。
 */
uint32_t mdx_loop_count(void) {
    return s_loops;
}

uint32_t mdx_reslip_count(void) {
    return s_reslips;
}

uint32_t mdx_tempo(void) {
    return s_tempo;
}

uint32_t mdx_tick_us(void) {
    return tick_us_for(s_tempo, opm_clock_hz_actual());
}

/* ---- 自己テスト -------------------------------------------------------- */

/*
 * 実機の音を使わずに確かめられるのはこの 2 つ。
 *  - 音程 -> KC/KF。KC の下位 4bit が 3/7/11/15 を飛ばす境界とオクターブ跨ぎ
 *  - Timer-B 値 -> 1 clock の長さ
 */
bool mdx_selftest(const char **detail) {
    static char msg[64];

    static const struct {
        uint16_t pitch;
        uint8_t kc;
        uint8_t kf;
    } PV[] = {
        {0u, 0x00u, 0x00u},          /* o0 d+ = KC 0 */
        {11u * 64u, 0x0eu, 0x00u},   /* オクターブの最後 */
        {12u * 64u, 0x10u, 0x00u},   /* 桁上がりで下位 4bit が 0 に戻る */
        {54u * 64u + 5u, 0x48u, 0x14u}, /* 調律ぶん +5/64 が KF に出る */
        {95u * 64u, 0x7eu, 0x00u},   /* 最高音 */
        {0x17ffu, 0x7eu, 0xfcu},     /* 上限 */
        {0x1800u, 0x7eu, 0xfcu},     /* 上限で頭打ち */
    };
    for (uint32_t i = 0; i < sizeof(PV) / sizeof(PV[0]); i++) {
        uint8_t kc, kf;
        pitch_to_kc_kf(PV[i].pitch, &kc, &kf);
        if (kc != PV[i].kc || kf != PV[i].kf) {
            snprintf(msg, sizeof(msg), "FAIL pitch %u -> %02x/%02x want %02x/%02x",
                     (unsigned)PV[i].pitch, kc, kf, PV[i].kc, PV[i].kf);
            *detail = msg;
            return false;
        }
    }

    static const struct {
        uint8_t tempo;
        uint32_t phim;
        uint32_t us;
    } TV[] = {
        {200u, 4000000u, 14336u},  /* よく使うテンポ */
        {255u, 4000000u, 256u},    /* いちばん速い */
        {0u, 4000000u, 65536u},    /* いちばん遅い */
        {190u, 4000000u, 16896u},
        {200u, 3579545u, 16019u},  /* φM を落とすと遅くなる */
    };
    for (uint32_t i = 0; i < sizeof(TV) / sizeof(TV[0]); i++) {
        uint32_t us = tick_us_for(TV[i].tempo, TV[i].phim);
        if (us != TV[i].us) {
            snprintf(msg, sizeof(msg), "FAIL @t %u -> %u us want %u",
                     (unsigned)TV[i].tempo, (unsigned)us, (unsigned)TV[i].us);
            *detail = msg;
            return false;
        }
    }

    *detail = "PASS (7 pitch, 5 tempo)";
    return true;
}

#else /* !MDX_ENABLED */

void mdx_init(void) {}
bool mdx_service(void) { return false; }
const char *mdx_play(const char *name) { (void)name; return "bad argument"; }
const char *mdx_stop(void) { return "wrong state"; }
const char *mdx_list(void (*tick)(void)) { (void)tick; return "wrong state"; }
mdx_state_t mdx_state(void) { return MDX_STATE_STOPPED; }
const char *mdx_state_name(void) { return "DISABLED"; }
bool mdx_is_playing(void) { return false; }
const char *mdx_current_name(void) { return ""; }
const char *mdx_title(void) { return ""; }
const char *mdx_pdx_name(void) { return ""; }
uint32_t mdx_channels(void) { return 0; }
uint64_t mdx_tick_count(void) { return 0; }
uint32_t mdx_loop_count(void) { return 0; }
uint32_t mdx_reslip_count(void) { return 0; }
uint32_t mdx_tempo(void) { return 0; }
uint32_t mdx_tick_us(void) { return 0; }
bool mdx_selftest(const char **detail) { *detail = "SKIP (disabled)"; return true; }

#endif /* MDX_ENABLED */
