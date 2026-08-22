/*
 * YM3012 (OPM の DAC) 出力のキャプチャと PCM 変換の実装
 *
 * SPDX-License-Identifier: MIT
 */
#include "ym3012.h"

#include <stdio.h>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "stats.h"
#include "ym3012.pio.h"

/* PCM は signed 16bit little-endian。int16_t の配列をそのまま送るのでこれが前提。 */
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "PCM 出力は little-endian 前提");

/*
 * リング本体。DMA のハードウェアリング機能はアドレスの下位ビットだけを回すので、
 * バッファをリングサイズと同じ境界へアラインしなければならない。
 */
static uint32_t s_ring[YM3012_RING_FRAMES]
    __attribute__((aligned(YM3012_RING_BYTES)));

/* キャプチャに使っている PIO / ステートマシン / DMA チャネル */
static PIO s_pio;
static uint s_sm;
static uint s_offset;
static uint s_dma_ch;

/*
 * DMA の書き込み位置はリング内の 12bit しか分からないので、差分を積んで
 * 64bit の総フレーム数へ延ばす。DMA 位置そのものを二重に持つわけではない。
 */
static uint32_t s_last_widx;
static uint64_t s_write_total;

/* USB キャプチャが使う既定の読み出しカーソル */
static ym3012_reader_t s_reader;

/* 変換直後に呼ぶミキサ（MDX の ADPCM パート）。未登録なら NULL。 */
static ym3012_mixer_t s_mixer;
static uint64_t s_mix_ready;

static bool s_selftest_ok;
static char s_selftest_detail[80];

/* ---- ループバック自己診断 ---------------------------------------------- */

/*
 * 無効ビット・仮数・指数から 32bit のフレームを組む。
 * 並びは受信順で bit0 から「無効 3 / 仮数 10 / 指数 3」を L・R の順に 2 組。
 */
static uint32_t frame_pack(uint32_t ul, uint32_t ml, uint32_t el,
                           uint32_t ur, uint32_t mr, uint32_t er) {
    return ((ul & 7u)) | ((ml & 0x3ffu) << 3) | ((el & 7u) << 13) |
           ((ur & 7u) << 16) | ((mr & 0x3ffu) << 19) | ((er & 7u) << 29);
}

#if YM3012_LOOPBACK_ENABLED

#define LOOPBACK_PIN_SO   (YM3012_LOOPBACK_BASE + 0u)
#define LOOPBACK_PIN_PHI1 (YM3012_LOOPBACK_BASE + 1u)
#define LOOPBACK_PIN_SH1  (YM3012_LOOPBACK_BASE + 2u)

/*
 * 合成フレームを 1 個ビットバンギングで流し込む。
 *
 * キャプチャ用 PIO は純粋にエッジ駆動でタイミング制約が無いので、CPU が
 * 好きな速さで叩いてよい。ここでは余裕を見て 1 クロック 2us で送る。
 */
static void loopback_send_frame(uint32_t word) {
    /* SH1 の立ち下がりでフレームの先頭を作る */
    gpio_put(LOOPBACK_PIN_SH1, 1);
    busy_wait_us_32(2);
    gpio_put(LOOPBACK_PIN_SH1, 0);
    busy_wait_us_32(2);

    /* YM3012 は LSB first。word の bit0 が最初に出る。 */
    for (uint i = 0; i < 32; i++) {
        gpio_put(LOOPBACK_PIN_SO, (word >> i) & 1u);
        gpio_put(LOOPBACK_PIN_PHI1, 0);
        busy_wait_us_32(1);
        gpio_put(LOOPBACK_PIN_PHI1, 1); /* この立ち上がりで PIO が SO を取り込む */
        busy_wait_us_32(1);
    }
}

/*
 * PIO のビット順・フレーム同期・L/R の並びを、外部機器なしで検証する。
 *
 * PIO は FUNCSEL に関係なくパッドの入力値を読めるので、ループバック用ピンは
 * SIO の出力にしたまま CPU から駆動してよい。DMA 開始前に 1 回だけ実行する。
 */
static bool loopback_selftest(void) {
    /* 期待する (L, R) は左右で別の値にして、入れ替わりを検出できるようにする */
    static const struct {
        uint32_t ul, ml, el; /* L の無効ビット・仮数・指数 */
        uint32_t ur, mr, er; /* R の無効ビット・仮数・指数 */
        int16_t expect_l;
        int16_t expect_r;
    } cases[] = {
        /* フルスケール両端。無効ビットが結果に混ざらないことも見る。 */
        {5u, 1023u, 7u, 2u, 0u, 7u, 32704, -32768},
        /* 最小ステップ。ゼロと +1 で L/R の取り違えが分かる。 */
        {7u, 512u, 1u, 0u, 513u, 1u, 0, 1},
        /* 負値と中間の指数。 */
        {3u, 511u, 1u, 4u, 768u, 2u, -1, 512},
    };

    for (uint i = 0; i < 3; i++) {
        gpio_init(YM3012_LOOPBACK_BASE + i);
        gpio_set_dir(YM3012_LOOPBACK_BASE + i, GPIO_OUT);
    }
    gpio_put(LOOPBACK_PIN_SO, 0);
    gpio_put(LOOPBACK_PIN_PHI1, 0);
    gpio_put(LOOPBACK_PIN_SH1, 1);

    ym3012_capture_program_init(s_pio, s_sm, s_offset, YM3012_LOOPBACK_BASE);
    pio_sm_clear_fifos(s_pio, s_sm);
    pio_sm_set_enabled(s_pio, s_sm, true);

    bool ok = true;
    for (uint i = 0; i < count_of(cases) && ok; i++) {
        uint32_t word = frame_pack(cases[i].ul, cases[i].ml, cases[i].el,
                                   cases[i].ur, cases[i].mr, cases[i].er);
        loopback_send_frame(word);

        if (pio_sm_is_rx_fifo_empty(s_pio, s_sm)) {
            snprintf(s_selftest_detail, sizeof(s_selftest_detail),
                     "FAIL case %u: no data pushed", i);
            ok = false;
            break;
        }

        uint32_t got = pio_sm_get(s_pio, s_sm);
        if (got != word) {
            snprintf(s_selftest_detail, sizeof(s_selftest_detail),
                     "FAIL case %u: word %08lx expect %08lx",
                     i, (unsigned long)got, (unsigned long)word);
            ok = false;
            break;
        }

        int16_t l, r;
        ym3012_frame_to_pcm(got, &l, &r);
        if (l != cases[i].expect_l || r != cases[i].expect_r) {
            snprintf(s_selftest_detail, sizeof(s_selftest_detail),
                     "FAIL case %u: pcm L=%d R=%d expect L=%d R=%d",
                     i, l, r, cases[i].expect_l, cases[i].expect_r);
            ok = false;
            break;
        }
    }

    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_sm_clear_fifos(s_pio, s_sm);

    /* ループバック用ピンは入力へ戻して手を引く */
    for (uint i = 0; i < 3; i++) {
        gpio_deinit(YM3012_LOOPBACK_BASE + i);
    }

    if (ok) {
        snprintf(s_selftest_detail, sizeof(s_selftest_detail), "PASS");
    }
    return ok;
}

#else /* YM3012_LOOPBACK_ENABLED */

static bool loopback_selftest(void) {
    snprintf(s_selftest_detail, sizeof(s_selftest_detail), "SKIP (disabled)");
    return true;
}

#endif /* YM3012_LOOPBACK_ENABLED */

/* ---- 初期化 ------------------------------------------------------------ */

void ym3012_init(void) {
    bool ok = pio_claim_free_sm_and_add_program(&ym3012_capture_program, &s_pio,
                                                &s_sm, &s_offset);
    hard_assert(ok);

    /* DMA を仕掛ける前にループバック自己診断を済ませる */
    s_selftest_ok = loopback_selftest();

    /* 本番の入力ピン。PIO は駆動しないので pindirs は入力のまま。 */
    for (uint pin = YM3012_PIN_SO; pin <= YM3012_PIN_SH2; pin++) {
        pio_gpio_init(s_pio, pin);
    }
    (void)pio_sm_set_consecutive_pindirs(s_pio, s_sm, YM3012_PIN_SO, 4, false);

    ym3012_capture_program_init(s_pio, s_sm, s_offset, YM3012_PIN_SO);
    pio_sm_clear_fifos(s_pio, s_sm);

    /*
     * PIO RX FIFO → リングの DMA。転送は 32bit 単位で 1 フレーム 1 エントリ。
     * TRANS_COUNT は ENDLESS にして、割り込みも停止も無しで回し続ける。
     */
    s_dma_ch = (uint)dma_claim_unused_channel(true);

    dma_channel_config c = dma_channel_get_default_config(s_dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, YM3012_RING_BITS);
    channel_config_set_dreq(&c, pio_get_dreq(s_pio, s_sm, false));

    dma_channel_configure(s_dma_ch, &c, s_ring, &s_pio->rxf[s_sm],
                          dma_encode_endless_transfer_count(), true);

    /* SM を動かす前なので、書き込み位置はまだリング先頭のまま */
    s_last_widx = 0;
    s_write_total = 0;
    ym3012_reader_init(&s_reader, true);

    pio_sm_set_enabled(s_pio, s_sm, true);
}

bool ym3012_selftest_passed(void) {
    return s_selftest_ok;
}

const char *ym3012_selftest_detail(void) {
    return s_selftest_detail;
}

/* ---- リング操作 -------------------------------------------------------- */

void ym3012_ring_poll(void) {
    uintptr_t wp = (uintptr_t)dma_channel_hw_addr(s_dma_ch)->write_addr;
    uint32_t widx = (uint32_t)((wp - (uintptr_t)s_ring) >> 2);

    /*
     * 前回からの進み分を足し込む。呼び出し間隔がリング一周 (65.5ms) より短い限り
     * この差分は一意に決まる。
     */
    s_write_total += (uint32_t)(widx - s_last_widx) & (YM3012_RING_FRAMES - 1u);
    s_last_widx = widx;
}

void ym3012_ring_resync(void) {
    /*
     * 差分を積まずに基準点だけ張り直す。ym3012_ring_poll() の差分はリング長
     * 4096 フレームで剰余を取るので、フラッシュ書き込みのように 65.5ms を超えて
     * 止まったあとに普通に呼ぶと、進んだ分が一周単位で切り捨てられて
     * s_write_total に恒久的なずれが残る。ここで基準を捨てて張り直す。
     *
     * 止まっている間に取り込まれたフレームは数えられないので、統計上は欠ける。
     */
    uintptr_t wp = (uintptr_t)dma_channel_hw_addr(s_dma_ch)->write_addr;
    s_last_widx = (uint32_t)((wp - (uintptr_t)s_ring) >> 2);

    /* 既定カーソルも現在位置へ寄せる（そうしないと誤 overrun になる） */
    ym3012_reader_sync(&s_reader);
}

uint64_t ym3012_write_total(void) {
    return s_write_total;
}

/* ---- 読み出しカーソル -------------------------------------------------- */

void ym3012_set_mixer(ym3012_mixer_t fn) {
    s_mixer = fn;
    s_mix_ready = s_write_total;
}

void ym3012_set_mix_ready(uint64_t frame) {
    s_mix_ready = frame;
}

void ym3012_reader_init(ym3012_reader_t *rd, bool count_forbidden) {
    rd->read_total = s_write_total;
    rd->count_forbidden = count_forbidden;
}

uint32_t ym3012_reader_unread(const ym3012_reader_t *rd) {
    uint64_t unread = s_write_total - rd->read_total;
    return (unread > YM3012_RING_FRAMES) ? YM3012_RING_FRAMES : (uint32_t)unread;
}

void ym3012_reader_sync(ym3012_reader_t *rd) {
    rd->read_total = s_write_total;
}

uint64_t ym3012_reader_read_total(const ym3012_reader_t *rd) {
    return rd->read_total;
}

uint32_t ym3012_reader_read_pcm(ym3012_reader_t *rd, int16_t *out, uint32_t max_frames) {
    uint32_t n = ym3012_reader_unread(rd);
    if (n > max_frames) {
        n = max_frames;
    }

    /* ミキサが描き終えたところまでしか渡さない（混ぜ損ねを作らない） */
    if (s_mixer != NULL) {
        if (rd->read_total >= s_mix_ready) {
            return 0u;
        }
        uint64_t ready = s_mix_ready - rd->read_total;
        if ((uint64_t)n > ready) {
            n = (uint32_t)ready;
        }
    }

    if (n == 0u) {
        return 0u;
    }

    /* リング末尾をまたぐ分は次回に回す（呼び出し側は残量を見て再度呼ぶ） */
    uint32_t ridx = (uint32_t)(rd->read_total & (YM3012_RING_FRAMES - 1u));
    uint32_t to_end = YM3012_RING_FRAMES - ridx;
    if (n > to_end) {
        n = to_end;
    }

    const uint32_t *src = &s_ring[ridx];
    uint32_t forbidden = 0;

    for (uint32_t i = 0; i < n; i++) {
        uint32_t frame = src[i];
        uint16_t wl = (uint16_t)frame;
        uint16_t wr = (uint16_t)(frame >> 16);

        /* 禁止コード E=0 の数はビット位相が正しいかの指標になる */
        if ((((uint32_t)wl >> 13) & 7u) == 0u) {
            forbidden++;
        }
        if ((((uint32_t)wr >> 13) & 7u) == 0u) {
            forbidden++;
        }

        out[2u * i] = ym3012_word_to_pcm(wl);       /* L */
        out[2u * i + 1u] = ym3012_word_to_pcm(wr);  /* R */
    }

    /*
     * 変換した PCM に別の音源を混ぜる（MDX の ADPCM パート）。
     * ここは USB キャプチャと I2S の唯一の合流点なので、1 箇所で両方に効く。
     */
    if (s_mixer != NULL) {
        s_mixer(rd->read_total, n, out);
    }

    rd->read_total += n;

    if (forbidden != 0u && rd->count_forbidden) {
        stats_count_forbidden(forbidden);
    }
    return n;
}

/* ---- 既定カーソル（USB キャプチャ用）---------------------------------- */

uint32_t ym3012_unread(void) {
    return ym3012_reader_unread(&s_reader);
}

void ym3012_ring_sync(void) {
    ym3012_reader_sync(&s_reader);
}

uint64_t ym3012_read_total(void) {
    return ym3012_reader_read_total(&s_reader);
}

uint32_t ym3012_read_pcm(int16_t *out, uint32_t max_frames) {
    return ym3012_reader_read_pcm(&s_reader, out, max_frames);
}

bool ym3012_check_rxstall(void) {
    uint32_t mask = 1u << (PIO_FDEBUG_RXSTALL_LSB + s_sm);
    if ((s_pio->fdebug & mask) == 0u) {
        return false;
    }
    s_pio->fdebug = mask; /* 1 を書いてクリア */
    return true;
}

/* ---- PCM 変換の自己テスト ---------------------------------------------- */

bool ym3012_pcm_selftest(const char **detail) {
    /* ワードは受信順 bit0 から「無効 3 / 仮数 10 / 指数 3」 */
    static const struct {
        uint32_t undef;
        uint32_t m;
        uint32_t e;
        int16_t expect;
    } cases[] = {
        /* 指数 1 = 最小ステップ。仮数のオフセットバイナリを確認する。 */
        {0u, 512u, 1u, 0},
        {0u, 513u, 1u, 1},
        {0u, 511u, 1u, -1},
        {0u, 0u, 1u, -512},
        {0u, 1023u, 1u, 511},
        /* 無効 3bit は結果に影響しないこと */
        {7u, 512u, 1u, 0},
        {5u, 1023u, 1u, 511},
        /* 指数を 1 上げるとステップが 2 倍になること */
        {0u, 512u, 2u, 0},
        {0u, 768u, 2u, 512},
        {0u, 256u, 2u, -512},
        /* 指数の全域 */
        {0u, 1023u, 2u, 1022},
        {0u, 1023u, 3u, 2044},
        {0u, 1023u, 4u, 4088},
        {0u, 1023u, 5u, 8176},
        {0u, 1023u, 6u, 16352},
        {0u, 1023u, 7u, 32704}, /* 正のフルスケール */
        {0u, 0u, 7u, -32768},   /* 負のフルスケール */
        {0u, 0u, 6u, -16384},
        {0u, 0u, 2u, -1024},
        /* E = 0 は禁止コード。無効ビットや仮数によらず 0。 */
        {0u, 512u, 0u, 0},
        {0u, 1023u, 0u, 0},
        {7u, 0u, 0u, 0},
    };

    static char msg[80];

    for (uint i = 0; i < count_of(cases); i++) {
        uint16_t w = (uint16_t)((cases[i].undef & 7u) |
                                ((cases[i].m & 0x3ffu) << 3) |
                                ((cases[i].e & 7u) << 13));
        int16_t got = ym3012_word_to_pcm(w);
        if (got != cases[i].expect) {
            snprintf(msg, sizeof(msg), "FAIL m=%u e=%u: %d expect %d",
                     (unsigned)cases[i].m, (unsigned)cases[i].e, got,
                     cases[i].expect);
            *detail = msg;
            return false;
        }
    }

    /*
     * 全域の不変量。E=1..7 で仮数を 1 増やすとちょうど 1<<(E-1) 増え、
     * 値域が -32768..+32704 に収まることを確認する。
     */
    for (uint32_t e = 1u; e <= 7u; e++) {
        int32_t step = (int32_t)1 << (e - 1u);
        for (uint32_t m = 0u; m < 1023u; m++) {
            uint16_t w0 = (uint16_t)((m << 3) | (e << 13));
            uint16_t w1 = (uint16_t)(((m + 1u) << 3) | (e << 13));
            int32_t d = (int32_t)ym3012_word_to_pcm(w1) -
                        (int32_t)ym3012_word_to_pcm(w0);
            if (d != step) {
                snprintf(msg, sizeof(msg), "FAIL step m=%u e=%u: %ld expect %ld",
                         (unsigned)m, (unsigned)e, (long)d, (long)step);
                *detail = msg;
                return false;
            }
        }
    }

    /* フレーム分解。下位 16bit が L、上位 16bit が R であること。 */
    uint32_t frame = frame_pack(7u, 1023u, 7u, 0u, 0u, 7u);
    int16_t l, r;
    ym3012_frame_to_pcm(frame, &l, &r);
    if (l != 32704 || r != -32768) {
        snprintf(msg, sizeof(msg), "FAIL frame: L=%d R=%d expect L=32704 R=-32768",
                 l, r);
        *detail = msg;
        return false;
    }

    *detail = "PASS";
    return true;
}
