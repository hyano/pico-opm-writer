/*
 * I2S 出力の実装
 *
 * SPDX-License-Identifier: MIT
 */
#include "i2s.h"

#if I2S_ENABLED

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pico/stdlib.h"

#include "i2s.pio.h"
#include "opm.h"
#include "stats.h"
#include "ym3012.h"

/*
 * ym3012_reader_read_pcm() が書く int16_t の並び (L, R) を、そのまま
 * uint32_t = (R << 16) | L として I2S へ流す。これが成り立つ前提。
 */
_Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__,
               "I2S 出力は little-endian 前提");

/* BCK と LRCK は sideset の連続 2bit に載せる */
_Static_assert(I2S_PIN_LRCK == I2S_PIN_BCK + 1, "LRCK は BCK の次のピンであること");

/* 先行量がリングを超えると自分で自分を踏む */
_Static_assert(I2S_TARGET_FRAMES < I2S_RING_FRAMES,
               "I2S_TARGET_FRAMES はリングより小さいこと");

/*
 * リング本体。DMA のハードウェアリング機能はアドレスの下位ビットだけを回すので、
 * バッファをリングサイズと同じ境界へアラインしなければならない。
 */
static uint32_t s_ring[I2S_RING_FRAMES] __attribute__((aligned(I2S_RING_BYTES)));

/* 出力に使っている PIO / ステートマシン / DMA チャネル */
static PIO s_pio;
static uint s_sm;
static uint s_offset;
static uint s_dma_ch;

/* PIO の分周比（16.8 固定小数） */
static uint32_t s_div256;

/* ソース側リングの読み出しカーソル。USB キャプチャとは独立に進む。 */
static ym3012_reader_t s_reader;

/*
 * DMA の読み出し位置はリング内の 12bit しか分からないので、差分を積んで
 * 64bit の総フレーム数へ延ばす。s_fill_total は CPU が書いた総フレーム数で、
 * 差が「DMA の先をどれだけ走っているか」= 出力レイテンシになる。
 */
static uint32_t s_last_ridx;
static uint64_t s_dma_total;
static uint64_t s_fill_total;

/*
 * 出力の有効・無効。無効中はソース側リングを一切読まず、無音だけを詰める。
 * ストレージが HOST モードのあいだ（PC が MSC でフラッシュを書いているあいだ）に
 * 使う。BCK / LRCK と DMA は止めない。止めると PCM5102A がポップするうえ、
 * 「I2S 出力は常時動作し停止しない」という前提が崩れる。
 */
static bool s_enabled = true;

/* ---- リング操作 -------------------------------------------------------- */

static void i2s_poll(void)
{
    uintptr_t rp = (uintptr_t)dma_channel_hw_addr(s_dma_ch)->read_addr;
    uint32_t ridx = (uint32_t)((rp - (uintptr_t)s_ring) >> 2);

    /* 呼び出し間隔がリング一周 (65.5ms) より短い限りこの差分は一意に決まる */
    s_dma_total += (uint32_t)(ridx - s_last_ridx) & (I2S_RING_FRAMES - 1u);
    s_last_ridx = ridx;
}

/* 書き込み位置から frames 個を無音で埋める */
static void fill_silence(uint32_t frames)
{
    while (frames > 0u)
    {
        uint32_t widx = (uint32_t)(s_fill_total & (I2S_RING_FRAMES - 1u));
        uint32_t n = I2S_RING_FRAMES - widx;
        if (n > frames)
        {
            n = frames;
        }
        for (uint32_t i = 0; i < n; i++)
        {
            s_ring[widx + i] = 0u;
        }
        s_fill_total += n;
        frames -= n;
    }
}

/* ---- 初期化 ------------------------------------------------------------ */

/*
 * 1 フレーム = 32bit = 64 SM サイクル、fs = φM/64 なので
 *   clkdiv_i2s = sys_clk / (64 x fs) = sys_clk / φM
 * φM 側は 2 命令のループなので clkdiv_opm = sys_clk / (2 x φM)、
 * つまり clkdiv_i2s = 2 x clkdiv_opm。256 倍固定小数のまま 2 倍すれば
 * 丸めが入らず、サンプリングレートがキャプチャ側と厳密に一致する。
 *
 * φM 側が何を入れていてもこの関係だけで決まるので、クロック切り替えの途中で
 * 使う切り上げ分周比にもそのまま追従する。
 */
static void i2s_compute_div(void)
{
    s_div256 = (((opm_clock_div_int() << 8) | opm_clock_div_frac()) * 2u);
}

void i2s_init(void)
{
    bool ok = pio_claim_free_sm_and_add_program(&i2s_out_program, &s_pio, &s_sm, &s_offset);
    hard_assert(ok);

    i2s_compute_div();

    i2s_out_program_init(s_pio, s_sm, s_offset, I2S_PIN_DIN, I2S_PIN_BCK,
                         s_div256 >> 8, (uint8_t)(s_div256 & 0xffu));

    /*
     * リング → PIO TX FIFO の DMA。転送は 32bit 単位で 1 フレーム 1 エントリ。
     * TRANS_COUNT は ENDLESS にして、割り込みも停止も無しで回し続ける。
     */
    s_dma_ch = (uint)dma_claim_unused_channel(true);

    dma_channel_config c = dma_channel_get_default_config(s_dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_ring(&c, false, I2S_RING_BITS); /* 読み出し側を回す */
    channel_config_set_dreq(&c, pio_get_dreq(s_pio, s_sm, true));

    dma_channel_configure(s_dma_ch, &c, &s_pio->txf[s_sm], s_ring,
                          dma_encode_endless_transfer_count(), false);

    /* リングは .bss なので全ゼロ = 無音。先行分を無音で埋めた状態から始める。 */
    s_last_ridx = 0;
    s_dma_total = 0;
    s_fill_total = I2S_TARGET_FRAMES;
    ym3012_reader_init(&s_reader, false);
    stats_i2s_update(I2S_TARGET_FRAMES);

    dma_channel_start(s_dma_ch);
    pio_sm_set_enabled(s_pio, s_sm, true);
}

/* ---- 毎周回の処理 ------------------------------------------------------ */

bool __not_in_flash_func(i2s_service)(void)
{
    i2s_poll();

    int64_t depth = (int64_t)(s_fill_total - s_dma_total);

    if (depth <= 0)
    {
        /*
         * アンダーラン。ENDLESS のリング DMA は止まらないので、DMA は無音ではなく
         * 古いリング内容を再生している。溜まった古いフレームを捨て、先行分を
         * 無音で埋め直して仕切り直す。
         */
        stats_count_i2s_underrun();
        ym3012_reader_sync(&s_reader);
        s_fill_total = s_dma_total;
        fill_silence(I2S_TARGET_FRAMES);
        stats_i2s_update(I2S_TARGET_FRAMES);
        return true;
    }

    stats_i2s_update((uint32_t)depth);

    if ((uint64_t)depth >= I2S_TARGET_FRAMES)
    {
        return false; /* 先行量は足りている */
    }

    uint32_t want = I2S_TARGET_FRAMES - (uint32_t)depth;

    if (!s_enabled)
    {
        /*
         * 無効中。ソースを読まずに無音で埋める。リング全体が無音になるので、
         * フラッシュ消去でサービスが止まって DMA が古い内容を再生しても無音のまま。
         */
        fill_silence(want);
        return true;
    }

    bool worked = false;

    while (want > 0u)
    {
        uint32_t widx = (uint32_t)(s_fill_total & (I2S_RING_FRAMES - 1u));
        uint32_t n = I2S_RING_FRAMES - widx; /* 出力リング末尾で切る */
        if (n > want)
        {
            n = want;
        }

        /*
         * ym3012_reader_read_pcm() は out[2i]=L / out[2i+1]=R と書く。
         * little-endian ではこれが uint32_t の (R << 16) | L になり、
         * i2s.pio が期待する並び（上位 16bit が先に出て R になる）と一致する。
         * 並べ替えは要らない。
         */
        n = ym3012_reader_read_pcm(&s_reader, (int16_t *)&s_ring[widx], n);
        if (n == 0u)
        {
            /*
             * ソースに未処理が無い。レートは厳密に一致しているので、ポーリングの
             * 位相差で一瞬空になるのは正常。ここで無音を差し込むと差し込んだ分だけ
             * ソース側にバックログが溜まり、最後は overrun するので何もしない。
             * 足りない分は次の周回で埋める。
             */
            break;
        }

        s_fill_total += n;
        want -= n;
        worked = true;
    }

    return worked;
}

/* ---- クロック切り替え -------------------------------------------------- */

void i2s_retune(void)
{
    i2s_compute_div();

    /*
     * 走ったまま SMx_CLKDIV を書くと進行中のカウントの扱いが規定されていないので、
     * 一瞬止めてから書く。停止中は BCK / LRCK が最後のレベルを保持するため、
     * H/L 期間は数百 ns 伸びるだけで短くはならない。
     *
     * pio_sm_restart() は使わない。PC を先頭へ戻すと X が中途半端なまま最初の
     * 位相のビット数がずれ、以後恒久的に位相が狂う（i2s.pio の注意書き）。
     * clkdiv_restart は分周カウンタを 0 に戻すだけで PC / X / Y には触らない。
     */
    pio_sm_set_enabled(s_pio, s_sm, false);
    pio_sm_set_clkdiv_int_frac8(s_pio, s_sm, s_div256 >> 8, (uint8_t)(s_div256 & 0xffu));
    pio_sm_clkdiv_restart(s_pio, s_sm);
    pio_sm_set_enabled(s_pio, s_sm, true);
}

/* ---- 停止をまたいだ復帰 ------------------------------------------------ */

void i2s_resync(void)
{
    /*
     * **差分は必ず積む。** s_dma_total と s_fill_total はどちらも
     * ≡ リング内の添字 (mod I2S_RING_FRAMES) で、fill_silence() も本体の書き込みも
     * この位相でリングを引いている。ここで基準点だけを張り直すと、直後の
     * s_fill_total = s_dma_total が CPU の書き込み位置を DMA の読み出し位置から
     * 止まっていた分だけ手前へ置いてしまい、真の先行量が I2S_TARGET_FRAMES ではなく
     * (I2S_TARGET_FRAMES - 止まっていた分) になる。depth は満量を報告し続けるので
     * depth <= 0 のアンダーラン復帰も発火せず、先行が尽きても気づけない。
     *
     * 位相さえ合っていれば、あとは i2s_service() のアンダーラン復帰とまったく同じ
     * 処理でよい（古いフレームを捨てて先行分を無音で埋め直す）。
     */
    i2s_poll();

    stats_count_i2s_underrun();
    ym3012_reader_sync(&s_reader);
    s_fill_total = s_dma_total;
    fill_silence(I2S_TARGET_FRAMES);
    stats_i2s_update(I2S_TARGET_FRAMES);
}

void i2s_set_enabled(bool enabled)
{
    s_enabled = enabled;
}

bool i2s_enabled(void)
{
    return s_enabled;
}

/* ---- 情報 -------------------------------------------------------------- */

uint32_t i2s_depth(void)
{
    int64_t depth = (int64_t)(s_fill_total - s_dma_total);
    return (depth <= 0) ? 0u : (uint32_t)depth;
}

uint32_t i2s_clkdiv_int(void)
{
    return s_div256 >> 8;
}

uint32_t i2s_clkdiv_frac(void)
{
    return s_div256 & 0xffu;
}

uint32_t i2s_rate_hz(void)
{
    /* fs = sys_clk / (64 x div256/256) = sys_clk x 4 / div256 */
    if (s_div256 == 0u)
    {
        return 0u;
    }
    return (uint32_t)(((uint64_t)clock_get_hz(clk_sys) * 4u) / s_div256);
}

uint32_t i2s_bck_hz(void)
{
    /* BCK = 32 x fs = sys_clk x 128 / div256 */
    if (s_div256 == 0u)
    {
        return 0u;
    }
    return (uint32_t)(((uint64_t)clock_get_hz(clk_sys) * 128u) / s_div256);
}

#else /* !I2S_ENABLED */

void i2s_init(void)
{
}

bool i2s_service(void)
{
    return false;
}

void i2s_retune(void)
{
}

void i2s_resync(void)
{
}

void i2s_set_enabled(bool enabled)
{
    (void)enabled;
}

bool i2s_enabled(void)
{
    return false;
}

uint32_t i2s_depth(void)
{
    return 0u;
}

uint32_t i2s_clkdiv_int(void)
{
    return 0u;
}

uint32_t i2s_clkdiv_frac(void)
{
    return 0u;
}

uint32_t i2s_rate_hz(void)
{
    return 0u;
}

uint32_t i2s_bck_hz(void)
{
    return 0u;
}

#endif /* I2S_ENABLED */
