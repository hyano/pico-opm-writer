/*
 * pico-opm-writer
 *
 * USB CDC のテキストコマンドから YM2151 (OPM) のレジスタを書き込む。
 *
 * SPDX-License-Identifier: MIT
 */
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "autoplay.h"
#include "button.h"
#include "capture.h"
#include "clockmode.h"
#include "flash_disk.h"
#include "i2s.h"
#include "led.h"
#include "mdx.h"
#include "opm.h"
#include "pcm8.h"
#include "stats.h"
#include "storage.h"
#include "usb_pcm.h"
#include "vgm.h"
#include "ym3012.h"

/* 1 行の最大長（これを超えたら ERR too long） */
#define LINE_MAX_LEN 255

/* `d` コマンドの上限 */
#define DELAY_MAX_MS 60000u

/* `p 0` のドレインを打ち切るまでの時間 */
#define DRAIN_TIMEOUT_MS 2000u

/* コマンド用 CDC のインスタンス番号（stdio が使う #0。PCM 出力は usb_pcm.h の #1） */
#define USB_CMD_CDC_ITF 0u

/*
 * 音声チェーン（リング位置の取り込み / ADPCM / キャプチャ送出 / I2S 供給）を
 * 回す最短間隔。
 *
 * フレームは φM/64 = 62500/s で届くので、毎周回回すと 1 回あたり 1 フレーム
 * 未満のために毎回ブロックの支度をすることになり、
 * 固定費だけで CPU の大半を使う（pcm8.c の PCM8_BATCH_FRAMES と同じ理屈を
 * チェーン全体へ広げたもの）。どのリングも一周 65.5ms あるので、この間隔なら
 * DMA の位置を見失わない。
 *
 * 大きくするほど固定費は下がるが、1 回で埋める量が増えて I2S の先行量の谷が
 * 深くなる。STATS_PROFILE=1 で測った実測（BOS14.MDX + USB キャプチャ、
 * サービス滞在時間の合計 us/s）:
 *
 *   間隔なし 620,718 / 100us 206,435 / 250us 160,238 / 500us 135,048 / 1000us 120,566
 *
 * 250us 以降は逓減し、1000us では SEQ LAG が 220us まで伸びる。500us を採る。
 * このとき 1 回で扱うのは 31 フレームで、I2S の先行 1024 フレームに対して十分小さい。
 */
#ifndef AUDIO_SERVICE_INTERVAL_US
#define AUDIO_SERVICE_INTERVAL_US 500u
#endif

/*
 * tud_task() を回す最短間隔。
 *
 * これも毎周回の固定費なので、音声チェーンだけを間引くとループが速く回るように
 * なったぶん呼び出し回数が増え、浮いた時間をそのまま食う（実測: 音声チェーンを
 * 250us 間隔にしたら LOOP が 3.4 倍になり USB の占有率が 15% -> 26% へ上がった）。
 *
 * **下限は PCM 出力のパケットレートで決まる。** tud_task() 1 回で CDC の
 * バルクエンドポイントへ載るのは EP バッファ 1 個ぶん (CFG_TUD_CDC_EP_BUFSIZE
 * = 64 バイト) なので、
 *
 *   下限 = 64 バイト / (phiM/64 x 4 バイト) = 64 / 250000 = 256us   (phiM 4MHz)
 *
 * を超えるとホストへ送り出す速さが取り込む速さに追いつかず、CDC #1 の送信 FIFO が
 * 詰まってキャプチャのリングが 65.5ms で overrun する。実測でも 500us にすると
 * `p 1` の直後に STATE が ERROR へ落ちた（250us でも USB_TX が 3968/4096 まで
 * 埋まって余裕が無い）。2 倍の余裕を見て 125us にしてある。
 *
 * phiM を下げるとレートも下がるので制約は緩む方向。上げる場合はここを見直すこと。
 */
#ifndef USB_TASK_INTERVAL_US
#define USB_TASK_INTERVAL_US 125u
#endif

/*
 * シーケンサ (VGM / MDX) の予定時刻を確かめる最短間隔。
 *
 * これも毎周回では多すぎる。MDX の 1 tick は最短でも 4ms 台、VGM の wait は
 * 最短 1 サンプル = 22.7us で、どちらも予算ループの中で遅れを取り返すので、
 * この間隔ぶんの遅れは `s` の SEQ LAG に乗るだけで発行そのものは詰まらない。
 * 音声チェーンより短くして、遅れが目に見えて増えないようにしてある。
 */
#ifndef SEQ_SERVICE_INTERVAL_US
#define SEQ_SERVICE_INTERVAL_US 50u
#endif

/*
 * ストレージの書き出しを確かめる最短間隔。
 *
 * 書き出しの判断は STORAGE_FLUSH_IDLE_MS (250ms) のアイドル期限なので、
 * 1ms 刻みで見れば足りる。HOST モードでなければ即座に返る。
 */
#ifndef STORAGE_SERVICE_INTERVAL_US
#define STORAGE_SERVICE_INTERVAL_US 1000u
#endif

/*
 * ボタン (GP21 / GP22) を読む最短間隔。
 *
 * デバウンスの窓 10ms に対して 10 サンプル取れて十分細かく、1 秒の長押し判定に
 * 対しては誤差 0.1% で十分粗い。人間には 1ms の遅れは見えない。
 */
#ifndef BUTTON_SERVICE_INTERVAL_US
#define BUTTON_SERVICE_INTERVAL_US 1000u
#endif

/* 接続状態の確認間隔。起動バナーはこの遅れの範囲で出れば足りる。 */
#ifndef CONNECT_POLL_INTERVAL_US
#define CONNECT_POLL_INTERVAL_US 100000u
#endif

static char s_line[LINE_MAX_LEN + 1];
static size_t s_len;
static bool s_overflow;

/*
 * 起動時にボタンで選んだ動作モードの結果。
 *
 * 起動直後の printf はホストが CDC を開く前なので捨てられる。後から USB を
 * 挿しても分かるよう、ここに控えて i の button 行で出す（組み立ては
 * button_boot_apply()）。
 */
static char s_boot_result[48] = "none";

/* 各グループを最後に回した時刻。上の *_INTERVAL_US の基準。 */
static uint32_t s_usb_last_us;
static uint32_t s_audio_last_us;
static uint32_t s_seq_last_us;
static uint32_t s_storage_last_us;
static uint32_t s_button_last_us;

/* ---- メインループの 1 周分 --------------------------------------------- */

/*
 * USB・キャプチャ・I2S・LED・統計をひと回しする。
 *
 * コマンドの待ち時間（`d` の遅延や `p 0` のドレイン待ち）の中からも呼ぶので、
 * 待っている間も PCM の送出・I2S への供給・USB の処理が止まらない。
 *
 * CPU 使用率は「サービス関数の中に居た時間」をそのまま積む。サービスは連続して
 * 呼ばれるので区間は 1 本で足りる。何もする事が無い周回はどれもすぐ返るので
 * 自然に 0 に近づき、USB の空き待ちで送れなかった周回も CPU としては数えない。
 * tud_task() は毎周回走る固定費でどのサービスの負荷でもないので、別に集計する。
 */
/*
 * 前回から interval_us 以上たっていたら true を返し、基準時刻を進める。
 *
 * 「最短でもこの間隔を空ける」であって「この周期ちょうどで回す」ではない。
 * メインループが長く止まったあとは 1 回だけ真になり、取りこぼしを溜めない。
 */
static bool due(uint32_t *last_us, uint32_t now_us, uint32_t interval_us)
{
    if ((uint32_t)(now_us - *last_us) < interval_us)
    {
        return false;
    }
    *last_us = now_us;
    return true;
}

static void service_all(void)
{
    uint32_t t0 = time_us_32();

    if (due(&s_usb_last_us, t0, USB_TASK_INTERVAL_US))
    {
        tud_task();
    }

    uint32_t t1 = time_us_32();

    /*
     * 音声チェーンは AUDIO_SERVICE_INTERVAL_US ごとにまとめて回す。
     *
     * 書き込み位置の取り込みと ADPCM のレンダリングは、どの消費者よりも先に
     * 行う。capture も I2S も ym3012_reader_read_pcm() の中でミックス済みの
     * PCM を受け取るので、そこまでに描き終わっていなければならない。
     * 間隔を空けても**この順序は変えない**。
     */
    if (due(&s_audio_last_us, t1, AUDIO_SERVICE_INTERVAL_US))
    {
        ym3012_ring_poll();
        STATS_SVC(STATS_SVC_PCM8, pcm8_service());

        STATS_SVC(STATS_SVC_CAPTURE, capture_service());
        STATS_SVC(STATS_SVC_I2S, i2s_service());
    }

    /*
     * シーケンサは音声チェーンより短い間隔で見る。予定時刻の判定そのものは軽いが、
     * 毎周回だと周回数ぶんの固定費がそのまま乗る。
     */
    if (due(&s_seq_last_us, t1, SEQ_SERVICE_INTERVAL_US))
    {
        STATS_SVC(STATS_SVC_VGM, vgm_service());
        STATS_SVC(STATS_SVC_MDX, mdx_service());
        /*
         * 曲送りはシーケンサの後。同じ周回で更新された再生状態をそのまま見られる。
         * 判定は比較が数回で済むので STATS_SVC では包まない。
         */
        autoplay_service();
    }

    if (due(&s_storage_last_us, t1, STORAGE_SERVICE_INTERVAL_US))
    {
        STATS_SVC(STATS_SVC_STORAGE, storage_service());
    }

    /*
     * ボタンは取り込むだけで、autoplay / storage は触らない。この関数はコマンド
     * 処理中からも再入的に呼ばれるので、ここから上位の操作を呼ぶと filelist の
     * 走査バッファを壊す。消化は main() のトップレベルの button_dispatch()。
     *
     * led_service() の直前に置いてあるのは、長押しが成立した周回のうちに
     * LED の合図を反映させるため。判定は比較が数回で済むので STATS_SVC では
     * 包まない（led_service() / autoplay_service() と同じ扱い）。
     */
    if (due(&s_button_last_us, t1, BUTTON_SERVICE_INTERVAL_US))
    {
        button_service();
    }

    led_service();

    stats_usb_busy_add(t1 - t0);
    stats_busy_add(time_us_32() - t1);
    stats_service();
}

/* ---- 応答 -------------------------------------------------------------- */

static void reply_ok(void)
{
    puts("OK");
}

static void reply_err(const char *reason)
{
    printf("ERR %s\n", reason);
}

static void print_info(void)
{
    printf("# pico-opm-writer %s\n", OPM_WRITER_VERSION);
    /* phiM を先に出す。`clock` の出力と並びを揃えるため。 */
    printf("# phiM    : %u Hz (clkdiv %u + %u/256)\n",
           (unsigned)opm_clock_hz_actual(),
           (unsigned)opm_clock_div_int(),
           (unsigned)opm_clock_div_frac());
    printf("# sys_clk : %u Hz\n", (unsigned)clock_get_hz(clk_sys));
    printf("# preset  : %s  (vgm %s)\n",
           clockmode_preset_name(clockmode_preset()),
           clockmode_auto() ? "auto" : "fixed");
    printf("# pins    : D0-D7=GP%d-GP%d A0=GP%d /CS=GP%d /WR=GP%d /RD=GP%d /IC=GP%d\n",
           OPM_PIN_D0, OPM_PIN_D0 + 7, OPM_PIN_A0, OPM_PIN_CS, OPM_PIN_WR, OPM_PIN_RD, OPM_PIN_IC);
    printf("# pins    : phiM=GP%d /IRQ=GP%d\n",
           OPM_PIN_PHIM, OPM_PIN_IRQ);
    printf("# timing  : t_wr=%dus t_addr=%dus t_data=%dus\n",
           OPM_T_WR_US, OPM_T_ADDR_US, OPM_T_DATA_US);
    printf("# ym3012  : SO=GP%d phi1=GP%d SH1=GP%d SH2=GP%d\n",
           YM3012_PIN_SO, YM3012_PIN_PHI1, YM3012_PIN_SH1, YM3012_PIN_SH2);
#if BUTTON_ENABLED
    printf("# pins    : SW1=GP%u SW2=GP%u (pull-up, active low)\n",
           (unsigned)BUTTON_PIN_SW1, (unsigned)BUTTON_PIN_SW2);
    printf("# button  : long %u ms  boot %s\n",
           (unsigned)(BUTTON_LONG_PRESS_US / 1000u), s_boot_result);
#else
    printf("# button  : disabled\n");
#endif
    printf("# capture : ring %u bytes (%u frames) rate %u Hz\n",
           (unsigned)YM3012_RING_BYTES, (unsigned)YM3012_RING_FRAMES,
           (unsigned)(opm_clock_hz_actual() / 64u));
#if I2S_ENABLED
    printf("# i2s     : BCK=GP%d LRCK=GP%d DIN=GP%d (clkdiv %u + %u/256)\n",
           I2S_PIN_BCK, I2S_PIN_LRCK, I2S_PIN_DIN,
           (unsigned)i2s_clkdiv_int(), (unsigned)i2s_clkdiv_frac());
    printf("# i2s     : 32fs bck %u Hz rate %u Hz latency %u frames (%u us)\n",
           (unsigned)i2s_bck_hz(), (unsigned)i2s_rate_hz(),
           (unsigned)I2S_TARGET_FRAMES,
           (unsigned)(i2s_rate_hz() ? (uint32_t)((uint64_t)I2S_TARGET_FRAMES * 1000000u /
                                                 i2s_rate_hz())
                                    : 0u));
#else
    printf("# i2s     : disabled\n");
#endif
    printf("# piotest : %s\n", ym3012_selftest_detail());
    printf("# storage : flash 0x%06x + %u KiB  cluster %u B  sector %u B\n",
           (unsigned)storage_region_offset(),
           (unsigned)(storage_region_size() / 1024u),
           (unsigned)FLASH_DISK_ES, (unsigned)FLASH_DISK_SS);
    printf("# vgm     : dir %s  rate %u Hz  budget %u us\n",
           VGM_DIR, (unsigned)VGM_SAMPLE_RATE, (unsigned)VGM_BUDGET_US);
    printf("# mdx     : dir %s  max %u KiB  budget %u us\n",
           MDX_DIR, (unsigned)(MDX_MAX_BYTES / 1024u), (unsigned)MDX_BUDGET_US);
    printf("# adpcm   : %s  %u ch  pdx stream %u bytes/ch\n",
           PCM8_ENABLED ? "enabled" : "disabled", (unsigned)PCM8_CH_MAX,
           (unsigned)PCM8_PREFETCH_BYTES);
    reply_ok();
}

/*
 * サービスごとの「呼ばれた回数」と「そのうち実仕事だった回数」を 1 行で出す。
 * どのサービスが空振りしているかは CPU 使用率からは読めないので、ここで分ける。
 * STATS_PROFILE=1 で焼いた場合だけ滞在時間の行が続く。
 */
static void print_svc(void)
{
    char line[192];
    size_t n = 0;

    for (uint32_t i = 0; i < (uint32_t)STATS_SVC_COUNT; i++)
    {
        stats_svc_t svc = (stats_svc_t)i;
        int w = snprintf(line + n, sizeof(line) - n, "%s%s %u/%u",
                         (i == 0u) ? "" : "  ", stats_svc_name(svc),
                         (unsigned)stats_svc_worked(svc), (unsigned)stats_svc_calls(svc));
        if (w < 0 || (size_t)w >= sizeof(line) - n)
        {
            break; /* 収まらない分は落とす（行を壊さない） */
        }
        n += (size_t)w;
    }
    printf("# SVC     : %s  worked/calls per s\n", line);

#if STATS_PROFILE
    n = 0;
    for (uint32_t i = 0; i < (uint32_t)STATS_SVC_COUNT; i++)
    {
        stats_svc_t svc = (stats_svc_t)i;
        int w = snprintf(line + n, sizeof(line) - n, "%s%s %u",
                         (i == 0u) ? "" : "  ", stats_svc_name(svc),
                         (unsigned)stats_svc_busy_us(svc));
        if (w < 0 || (size_t)w >= sizeof(line) - n)
        {
            break;
        }
        n += (size_t)w;
    }
    printf("# SVCTIME : %s  us per s\n", line);
#endif
}

/* `s` の出力。単位はすべてバイト。 */
static void print_stats(void)
{
    uint32_t ring = stats_ring_frames() * 4u;
    uint32_t ring_max = stats_ring_frames_max() * 4u;
    uint32_t tx_cap = usb_pcm_capacity();

    printf("# STATE   : %s\n", capture_state_name());
    printf("# CPU     : %u%% (max %u%%)   USB %u%%\n",
           (unsigned)stats_cpu_percent(), (unsigned)stats_cpu_percent_max(),
           (unsigned)stats_usb_percent());
    printf("# RING    : %u/%u bytes  MAX %u/%u  FREE %u\n",
           (unsigned)ring, (unsigned)YM3012_RING_BYTES,
           (unsigned)ring_max, (unsigned)YM3012_RING_BYTES,
           (unsigned)(YM3012_RING_BYTES - ring));
    printf("# USB_TX  : %u/%u bytes  MAX %u/%u\n",
           (unsigned)stats_usb_tx_bytes(), (unsigned)tx_cap,
           (unsigned)stats_usb_tx_bytes_max(), (unsigned)tx_cap);
#if I2S_ENABLED
    uint32_t i2s_min = stats_i2s_depth_min();
    printf("# I2S     : depth %u/%u frames  MIN %u  UNDERRUN %u\n",
           (unsigned)stats_i2s_depth(), (unsigned)I2S_TARGET_FRAMES,
           (unsigned)(i2s_min == UINT32_MAX ? 0u : i2s_min),
           (unsigned)stats_i2s_underrun());
#endif
    printf("# OVERRUN : %u   E0 : %llu   RXSTALL : %u\n",
           (unsigned)stats_overrun(), (unsigned long long)stats_forbidden(),
           (unsigned)stats_rxstall());
    printf("# RATE    : %u frames/s (expect %u)\n",
           (unsigned)stats_frame_rate(), (unsigned)(opm_clock_hz_actual() / 64u));
    printf("# LOOP    : %u passes/s\n", (unsigned)stats_loop_rate());
    print_svc();
    printf("# FRAMES  : %llu\n", (unsigned long long)stats_frames());
    printf("# FLASH   : WRITE %u   BLACKOUT max %u us\n",
           (unsigned)stats_flash_write(), (unsigned)stats_flash_blackout_max_us());
    const char *vgm_name = vgm_current_name();
    printf("# VGM     : %s%s%s%s\n", vgm_state_name(), vgm_name[0] ? " " : "", vgm_name,
           vgm_is_compressed() ? "  (gzip)" : "");
    printf("# VGM POS : %llu/%u samples  loop %u\n",
           (unsigned long long)vgm_position_samples(), (unsigned)vgm_total_samples(),
           (unsigned)vgm_loop_count());
    printf("# VGM LAG : reslip %u  gz reload %u\n",
           (unsigned)vgm_reslip_count(), (unsigned)vgm_gz_reload_count());
    const char *mdx_name = mdx_current_name();
    printf("# MDX     : %s%s%s\n", mdx_state_name(), mdx_name[0] ? " " : "", mdx_name);
    printf("# MDX POS : %llu clocks  loop %u  ch %u\n",
           (unsigned long long)mdx_tick_count(), (unsigned)mdx_loop_count(),
           (unsigned)mdx_channels());
    printf("# MDX TICK: @t %u  %u us  reslip %u\n",
           (unsigned)mdx_tempo(), (unsigned)mdx_tick_us(), (unsigned)mdx_reslip_count());
    printf("# MDX PCM : %s  %u/%u ch  mask %02x  keyon %u  miss %u  reads %u  CLIP %llu\n",
           pcm8_enabled() ? "on" : "off", (unsigned)pcm8_active_count(),
           (unsigned)PCM8_CH_MAX, (unsigned)pcm8_active_mask(),
           (unsigned)pcm8_keyon_count(), (unsigned)pcm8_miss_count(),
           (unsigned)pcm8_read_count(), (unsigned long long)stats_pcm_clip());
    /* スケジューラの遅れは VGM と MDX で共用（同時に走らないので 1 個で足りる） */
    printf("# SEQ LAG : max %u us\n", (unsigned)stats_seq_lag_max_us());
    printf("# PIOTEST : %s\n", ym3012_selftest_detail());
    printf("# IRQ     : %s\n", opm_irq_level() ? "H" : "L");
    reply_ok();
}

static void print_help(void)
{
    puts("# w <addr> <data> [<addr> <data> ...] : write register(s), hex 00-ff");
    puts("# r                                   : hardware reset (/IC)");
    puts("# c                                   : clear all registers (software)");
    puts("# d <ms>                              : delay, decimal 0-60000");
    puts("# p | p 1 | p 0                       : show / start / stop PCM output on CDC #1");
    puts("# s | s 0                             : show / reset statistics");
    puts("# t                                   : run self tests (pcm / piotest / mdx / adpcm)");
    puts("# i                                   : show info");
    puts("# clock | clock status                : show phiM / sys_clk / i2s rate");
    puts("# clock 4 | clock 3.58                : switch phiM to 4.000000 / 3.579545 MHz");
    puts("# clock auto | clock fixed            : follow / ignore the clock a file asks for");
    puts("# storage | storage status            : show storage state");
    puts("# storage host | storage player       : hand the flash to PC / to firmware");
    puts("# storage format [force] yes          : make a new filesystem (FAT12)");
    puts("# storage trace                       : show the SCSI commands the PC sent");
    puts("# vgm | vgm status                    : show VGM playback state");
    puts("# vgm list                            : list /VGM/**/*.vgm and *.vgz");
    puts("# vgm play <path>                     : play /VGM/<path>, subfolders ok");
    puts("# vgm stop                            : stop playback");
    puts("# mdx | mdx status                    : show MDX playback state");
    puts("# mdx list                            : list /MDX/**/*.mdx");
    puts("# mdx play <path>                     : play /MDX/<path>, subfolders ok");
    puts("# mdx stop                            : stop playback");
    puts("# mdx pcm | mdx pcm on | mdx pcm off  : show / toggle ADPCM (PCM8) mixing");
    puts("# autoplay | autoplay status          : show autoplay state");
    puts("# autoplay list                       : show the playlist");
    puts("# autoplay start | autoplay stop      : start / stop unattended playback");
    puts("# autoplay next | autoplay prev       : skip to the next / previous track");
    puts("# autoplay mode list | mode random    : play in list order / shuffled");
    puts("# autoplay loop <n>                   : loops before fade-out, 0 = endless");
    puts("# autoplay fade <ms> | gap <ms>       : fade-out / silence between tracks");
    puts("# autoplay source vgm | mdx | both    : which directories to play");
    puts("# h | ? | help                        : show this help");
    reply_ok();
}

/* ---- トークン分割 ------------------------------------------------------ */

/* 空白 / タブ区切りで次のトークンを切り出す。無ければ NULL。 */
static char *next_token(char **cursor)
{
    char *p = *cursor;

    while (*p == ' ' || *p == '\t')
    {
        p++;
    }
    if (*p == '\0')
    {
        *cursor = p;
        return NULL;
    }

    char *start = p;
    while (*p != '\0' && *p != ' ' && *p != '\t')
    {
        p++;
    }
    if (*p != '\0')
    {
        *p = '\0';
        p++;
    }

    *cursor = p;
    return start;
}

/*
 * 残りの行をまるごと 1 引数として返す。前後の空白は落とす。空なら NULL。
 * next_token() と違って区切らないので、空白を含むファイル名をそのまま扱える。
 */
static char *rest_of_line(char **cursor)
{
    char *p = *cursor;

    while (*p == ' ' || *p == '\t')
    {
        p++;
    }
    if (*p == '\0')
    {
        *cursor = p;
        return NULL;
    }

    char *start = p;
    char *end = p + strlen(p);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
    {
        end--;
    }
    *end = '\0';

    *cursor = end;
    return start;
}

/* トークンの大小無視比較。strcasecmp に依存しない。 */
static bool tok_is(const char *tok, const char *name)
{
    for (;;)
    {
        int a = tolower((unsigned char)*tok);
        int b = tolower((unsigned char)*name);
        if (a != b)
        {
            return false;
        }
        if (a == '\0')
        {
            return true;
        }
        tok++;
        name++;
    }
}

/* ---- 引数のパース ------------------------------------------------------ */

/* エラー時はエラー理由の文字列、成功時は NULL を返す。 */

static const char *parse_hex_u8(const char *s, uint8_t *out)
{
    uint32_t v = 0;
    bool any = false;

    for (const char *p = s; *p != '\0'; p++)
    {
        int c = tolower((unsigned char)*p);
        int digit;
        if (c >= '0' && c <= '9')
        {
            digit = c - '0';
        }
        else if (c >= 'a' && c <= 'f')
        {
            digit = c - 'a' + 10;
        }
        else
        {
            return "bad argument";
        }
        v = v * 16u + (uint32_t)digit;
        if (v > 0xffu)
        {
            return "out of range";
        }
        any = true;
    }
    if (!any)
    {
        return "bad argument";
    }

    *out = (uint8_t)v;
    return NULL;
}

static const char *parse_dec_u32(const char *s, uint32_t max, uint32_t *out)
{
    uint32_t v = 0;
    bool any = false;

    for (const char *p = s; *p != '\0'; p++)
    {
        if (*p < '0' || *p > '9')
        {
            return "bad argument";
        }
        v = v * 10u + (uint32_t)(*p - '0');
        if (v > max)
        {
            return "out of range";
        }
        any = true;
    }
    if (!any)
    {
        return "bad argument";
    }

    *out = v;
    return NULL;
}

/* ---- コマンド ---------------------------------------------------------- */

/* 引数を取らないコマンド用。余分なトークンがあれば ERR を返して false。 */
static bool expect_no_args(char **cursor)
{
    if (next_token(cursor) != NULL)
    {
        reply_err("wrong arity");
        return false;
    }
    return true;
}

/*
 * VGM 再生中はレジスタを直接叩かせない。VGM とユーザーの書き込みが混ざると
 * 何が鳴っているのか分からなくなるうえ、チップの状態は VGM 側が持っている。
 * 拒否したら true。
 */
static bool reject_while_playing(void)
{
    if (vgm_is_playing())
    {
        printf("# hint    : VGM is playing; run vgm stop first\n");
        reply_err("wrong state");
        return true;
    }
    if (mdx_is_playing())
    {
        printf("# hint    : MDX is playing; run mdx stop first\n");
        reply_err("wrong state");
        return true;
    }
    return false;
}

/*
 * w <addr> <data> [<addr> <data> ...]
 *
 * ペアを読むそばから書き込むため、途中でエラーになってもそこまでの書き込みは
 * 実行済みのまま ERR を返す（仕様どおり）。
 */
static void cmd_write(char **cursor)
{
    if (reject_while_playing())
    {
        return;
    }

    int pairs = 0;

    for (;;)
    {
        char *tok_addr = next_token(cursor);
        if (tok_addr == NULL)
        {
            if (pairs == 0)
            {
                reply_err("wrong arity");
            }
            else
            {
                reply_ok();
            }
            return;
        }

        char *tok_data = next_token(cursor);
        if (tok_data == NULL)
        {
            reply_err("wrong arity");
            return;
        }

        uint8_t addr, data;
        const char *err = parse_hex_u8(tok_addr, &addr);
        if (err == NULL)
        {
            err = parse_hex_u8(tok_data, &data);
        }
        if (err != NULL)
        {
            reply_err(err);
            return;
        }

        opm_write(addr, data);
        pairs++;
    }
}

static void cmd_delay(char **cursor)
{
    char *tok = next_token(cursor);
    if (tok == NULL)
    {
        reply_err("wrong arity");
        return;
    }
    if (!expect_no_args(cursor))
    {
        return;
    }

    uint32_t ms;
    const char *err = parse_dec_u32(tok, DELAY_MAX_MS, &ms);
    if (err != NULL)
    {
        reply_err(err);
        return;
    }

    /*
     * sleep_ms() で止めるとキャプチャ中に DMA リングが溢れる（16KB = 65.5ms 分しかない）。
     * 待っている間もサービスを回す。
     */
    absolute_time_t deadline = make_timeout_time_ms(ms);
    while (!time_reached(deadline))
    {
        service_all();
    }
    reply_ok();
}

/*
 * p 1 / p 0
 *
 * PIO と DMA は常時動いているので、ここで制御しているのは CDC #1 への送信だけ。
 */
/* `p`（引数なし）の出力。キャプチャの可否をこれだけで判断できるようにする。 */
static void print_capture_status(void)
{
    printf("# capture : %s\n", capture_state_name());
    printf("# cdc1    : %s\n", usb_pcm_connected() ? "connected" : "not connected");
    printf("# rate    : %u Hz\n", (unsigned)(opm_clock_hz_actual() / 64u));
    reply_ok();
}

static void cmd_pcm(char **cursor)
{
    char *tok = next_token(cursor);
    if (tok == NULL)
    {
        print_capture_status();
        return;
    }
    if (!expect_no_args(cursor))
    {
        return;
    }

    uint32_t mode;
    const char *err = parse_dec_u32(tok, 1u, &mode);
    if (err != NULL)
    {
        reply_err(err);
        return;
    }

    if (mode == 1u)
    {
        /*
         * HOST モード中はフラッシュの消去でメインループが数十 ms 止まるので、
         * キャプチャを走らせない。判定はここに置き、capture.c に storage への
         * 依存を持ち込まない。
         */
        if (storage_mode() == STORAGE_MODE_HOST)
        {
            printf("# hint    : cannot capture PCM in storage host; run storage player first\n");
            reply_err("wrong state");
            return;
        }
        err = capture_start();
        if (err != NULL)
        {
            reply_err(err);
        }
        else
        {
            printf("# capture : %s\n", capture_state_name());
            reply_ok();
        }
        return;
    }

    err = capture_request_stop();
    if (err != NULL)
    {
        reply_err(err);
        return;
    }

    /*
     * p 0 時点までの残りを送り切るまで待つ。待っている間も service_all() を回すので
     * DMA も USB も止まらない。ホストが CDC #1 を読まない場合に備えて上限を設ける。
     */
    absolute_time_t deadline = make_timeout_time_ms(DRAIN_TIMEOUT_MS);
    while (capture_state() == CAPTURE_STATE_DRAINING)
    {
        service_all();
        if (time_reached(deadline))
        {
            capture_abort();
            reply_err("drain timeout");
            return;
        }
    }
    printf("# capture : %s\n", capture_state_name());
    reply_ok();
}

/* s（統計表示） / s 0（統計リセット） */
static void cmd_stats(char **cursor)
{
    char *tok = next_token(cursor);
    if (tok == NULL)
    {
        print_stats();
        return;
    }
    if (!expect_no_args(cursor))
    {
        return;
    }

    uint32_t v;
    const char *err = parse_dec_u32(tok, 0u, &v);
    if (err != NULL)
    {
        reply_err(err);
        return;
    }

    stats_reset();
    pcm8_reset_counters(); /* `s` の MDX PCM 行はここも含めてまとめて 0 に戻す */
    reply_ok();
}

/* ---- storage -------------------------------------------------------- */

static void print_storage_status(void)
{
    printf("# storage : %s\n", storage_mode_name());
    printf("# medium  : %s\n", storage_medium_present() ? "present" : "not present");
    printf("# audio   : %s\n",
           (storage_mode() == STORAGE_MODE_HOST) ? "disabled (host mode)" : "enabled");
    printf("# region  : flash 0x%06x + %u KiB  (LBA %u B x %u)\n",
           (unsigned)storage_region_offset(),
           (unsigned)(storage_region_size() / 1024u),
           (unsigned)FLASH_DISK_SS, (unsigned)FLASH_DISK_LBA_COUNT);

    uint32_t fw_end = storage_firmware_end();
    uint32_t gap = (fw_end < storage_region_offset()) ? (storage_region_offset() - fw_end) : 0u;
    printf("# firmware: end 0x%08x (%u B)  gap %u KiB\n",
           (unsigned)(XIP_BASE + fw_end), (unsigned)fw_end, (unsigned)(gap / 1024u));

    uint32_t free_kib = 0, total_kib = 0;
    if (storage_space_kib(&free_kib, &total_kib))
    {
        printf("# fs      : %s  cluster %u B  free %u/%u KiB\n",
               storage_fs_type_name(), (unsigned)storage_cluster_bytes(),
               (unsigned)free_kib, (unsigned)total_kib);
    }
    else
    {
        printf("# fs      : %s\n", storage_fs_state_name());
    }

    printf("# label   : %s\n", storage_label()[0] ? storage_label() : "-");
    printf("# cache   : %u lines  dirty %u\n",
           (unsigned)FLASH_DISK_CACHE_LINES, (unsigned)flash_disk_dirty_lines());
    printf("# flash   : WRITE %u   BLACKOUT max %u us\n",
           (unsigned)stats_flash_write(), (unsigned)stats_flash_blackout_max_us());
    reply_ok();
}

/* `clock` の出力。切り替え後の確認にも使う。 */
static void print_clock_status(void)
{
    printf("# phiM    : %u Hz (clkdiv %u + %u/256)\n",
           (unsigned)opm_clock_hz_actual(),
           (unsigned)opm_clock_div_int(), (unsigned)opm_clock_div_frac());
    printf("# sys_clk : %u Hz\n", (unsigned)clock_get_hz(clk_sys));
    printf("# preset  : %s  (vgm %s)\n",
           clockmode_preset_name(clockmode_preset()),
           clockmode_auto() ? "auto" : "fixed");
#if I2S_ENABLED
    printf("# i2s     : clkdiv %u + %u/256  rate %u Hz  bck %u Hz\n",
           (unsigned)i2s_clkdiv_int(), (unsigned)i2s_clkdiv_frac(),
           (unsigned)i2s_rate_hz(), (unsigned)i2s_bck_hz());
#endif
    printf("# capture : rate %u Hz\n", (unsigned)(opm_clock_hz_actual() / 64u));
    reply_ok();
}

/*
 * clock | clock 4 | clock 3.58 | clock auto | clock fixed
 *
 * VGM 再生中でも許す。レジスタを叩くわけではないので reject_while_playing() は
 * 使わない（音程が変わるだけで、テンポは time_us_64() 基準なので狂わない）。
 */
static void cmd_clock(char **cursor)
{
    char *sub = next_token(cursor);
    if (sub == NULL)
    {
        print_clock_status();
        return;
    }
    if (!expect_no_args(cursor))
    {
        return;
    }

    if (tok_is(sub, "status"))
    {
        print_clock_status();
        return;
    }

    if (tok_is(sub, "auto") || tok_is(sub, "fixed"))
    {
        clockmode_set_auto(tok_is(sub, "auto"));
        reply_ok();
        return;
    }

    clock_preset_t target;
    if (tok_is(sub, clockmode_preset_name(CLOCK_PRESET_4MHZ)))
    {
        target = CLOCK_PRESET_4MHZ;
    }
    else if (tok_is(sub, clockmode_preset_name(CLOCK_PRESET_NTSC)))
    {
        target = CLOCK_PRESET_NTSC;
    }
    else
    {
        /* プリセット名は「未知のサブコマンド」であって解釈できない数値ではない */
        reply_err("unknown command");
        return;
    }

    const char *err = clockmode_set(target);
    if (err != NULL)
    {
        reply_err(err);
        return;
    }
    print_clock_status();
}

/*
 * storage status | host | player | format yes
 *
 * format は確認トークンを必須にする。すでにマウントできている領域を消すときは
 * format force yes を要求する。
 */
static void cmd_storage(char **cursor)
{
    char *sub = next_token(cursor);
    if (sub == NULL)
    {
        print_storage_status();
        return;
    }

    if (tok_is(sub, "trace"))
    {
        if (!expect_no_args(cursor))
        {
            return;
        }
        usb_msc_trace_dump();
        reply_ok();
        return;
    }

    if (tok_is(sub, "status"))
    {
        if (expect_no_args(cursor))
        {
            print_storage_status();
        }
        return;
    }

    if (tok_is(sub, "host"))
    {
        if (!expect_no_args(cursor))
        {
            return;
        }
        const char *err = storage_set_host();
        if (err != NULL)
        {
            reply_err(err);
        }
        else
        {
            printf("# storage : %s\n", storage_mode_name());
            reply_ok();
        }
        return;
    }

    if (tok_is(sub, "player"))
    {
        if (!expect_no_args(cursor))
        {
            return;
        }
        const char *err = storage_set_player();
        if (err != NULL)
        {
            reply_err(err);
        }
        else
        {
            printf("# storage : %s\n", storage_mode_name());
            reply_ok();
        }
        return;
    }

    if (tok_is(sub, "format"))
    {
        char *tok = next_token(cursor);
        bool force = false;
        if (tok != NULL && tok_is(tok, "force"))
        {
            force = true;
            tok = next_token(cursor);
        }
        if (tok == NULL || !tok_is(tok, "yes"))
        {
            reply_err("bad argument");
            return;
        }
        if (!expect_no_args(cursor))
        {
            return;
        }
        if (storage_mode() != STORAGE_MODE_PLAYER)
        {
            printf("# hint    : return to storage player before formatting\n");
            reply_err("wrong state");
            return;
        }
        if (!force && storage_fs_state() == STORAGE_FS_MOUNTED)
        {
            printf("# hint    : a filesystem already exists; use storage format force yes to erase it\n");
            reply_err("wrong state");
            return;
        }

        /* ガードを全部抜けてから進捗を出す */
        printf("# format  : takes tens of seconds; I2S will underrun meanwhile\n");
        const char *err = storage_format();
        if (err != NULL)
        {
            reply_err(err);
        }
        else
        {
            reply_ok();
        }
        return;
    }

    reply_err("unknown command");
}

/* ---- vgm ------------------------------------------------------------ */

/*
 * vgm list | play <path> | stop
 *
 * ファイル名は行の残り全部を 1 引数として受けるので、空白を含む名前も扱える。
 */
/* `vgm`（引数なし）と `vgm status` の出力。統計本体は `s` に置いたまま。 */
static void print_vgm_status(void)
{
    const char *name = vgm_current_name();
    printf("# vgm     : %s%s%s%s\n", vgm_state_name(), name[0] ? " " : "", name,
           vgm_is_compressed() ? "  (gzip)" : "");
    printf("# pos     : %llu/%u samples  loop %u\n",
           (unsigned long long)vgm_position_samples(), (unsigned)vgm_total_samples(),
           (unsigned)vgm_loop_count());
    printf("# lag     : reslip %u  gz reload %u\n",
           (unsigned)vgm_reslip_count(), (unsigned)vgm_gz_reload_count());
    reply_ok();
}

static void cmd_vgm(char **cursor)
{
    char *sub = next_token(cursor);
    if (sub == NULL)
    {
        print_vgm_status();
        return;
    }

    if (tok_is(sub, "status"))
    {
        if (expect_no_args(cursor))
        {
            print_vgm_status();
        }
        return;
    }

    if (tok_is(sub, "list"))
    {
        if (!expect_no_args(cursor))
        {
            return;
        }
        /* 行数が多いと printf の待ちが積もるので、1 行ごとにサービスを回す。 */
        const char *err = vgm_list(service_all);
        if (err != NULL)
        {
            reply_err(err);
        }
        else
        {
            reply_ok();
        }
        return;
    }

    if (tok_is(sub, "play"))
    {
        char *name = rest_of_line(cursor);
        if (name == NULL)
        {
            reply_err("wrong arity");
            return;
        }
        /* 手で曲を選んだら自動再生は降りる（曲送りと食い違わせない） */
        autoplay_stop();
        const char *err = vgm_play(name);
        if (err != NULL)
        {
            reply_err(err);
        }
        else
        {
            reply_ok();
        }
        return;
    }

    if (tok_is(sub, "stop"))
    {
        if (!expect_no_args(cursor))
        {
            return;
        }
        autoplay_stop();
        const char *err = vgm_stop();
        if (err != NULL)
        {
            reply_err(err);
        }
        else
        {
            reply_ok();
        }
        return;
    }

    reply_err("unknown command");
}

/*
 * mdx list | play <path> | stop
 *
 * vgm と同じく、ファイル名は行の残り全部を 1 引数として受ける。
 */
/* `mdx`（引数なし）と `mdx status` の出力。ADPCM の詳細は `mdx pcm` の方に置く。 */
static void print_mdx_status(void)
{
    const char *name = mdx_current_name();
    printf("# mdx     : %s%s%s\n", mdx_state_name(), name[0] ? " " : "", name);
    if (mdx_title()[0] != '\0')
    {
        printf("# title   : %s\n", mdx_title());
    }
    printf("# pos     : %llu clocks  loop %u  ch %u\n",
           (unsigned long long)mdx_tick_count(), (unsigned)mdx_loop_count(),
           (unsigned)mdx_channels());
    printf("# tick    : @t %u  %u us  reslip %u\n",
           (unsigned)mdx_tempo(), (unsigned)mdx_tick_us(), (unsigned)mdx_reslip_count());
    reply_ok();
}

static void cmd_mdx(char **cursor)
{
    char *sub = next_token(cursor);
    if (sub == NULL)
    {
        print_mdx_status();
        return;
    }

    if (tok_is(sub, "status"))
    {
        if (expect_no_args(cursor))
        {
            print_mdx_status();
        }
        return;
    }

    if (tok_is(sub, "list"))
    {
        if (!expect_no_args(cursor))
        {
            return;
        }
        const char *err = mdx_list(service_all);
        if (err != NULL)
        {
            reply_err(err);
        }
        else
        {
            reply_ok();
        }
        return;
    }

    if (tok_is(sub, "play"))
    {
        char *name = rest_of_line(cursor);
        if (name == NULL)
        {
            reply_err("wrong arity");
            return;
        }
        autoplay_stop();
        const char *err = mdx_play(name);
        if (err != NULL)
        {
            reply_err(err);
        }
        else
        {
            reply_ok();
        }
        return;
    }

    if (tok_is(sub, "stop"))
    {
        if (!expect_no_args(cursor))
        {
            return;
        }
        autoplay_stop();
        const char *err = mdx_stop();
        if (err != NULL)
        {
            reply_err(err);
        }
        else
        {
            reply_ok();
        }
        return;
    }

    if (tok_is(sub, "pcm"))
    {
        char *arg = next_token(cursor);
        if (arg != NULL)
        {
            bool on;
            if (tok_is(arg, "on"))
            {
                on = true;
            }
            else if (tok_is(arg, "off"))
            {
                on = false;
            }
            else
            {
                reply_err("bad argument");
                return;
            }
            if (!expect_no_args(cursor))
            {
                return;
            }
            /*
             * デコーダごとリンクされていない構成では on にしても鳴らないので、
             * 黙って OK を返さずに理由を出す（状態表示の方は通す）。
             */
            if (!PCM8_ENABLED)
            {
                printf("# hint    : ADPCM is disabled at build time (PCM8_ENABLED=0)\n");
                reply_err("unsupported");
                return;
            }
            pcm8_set_enabled(on);
        }

        static const char *const PAN_NAME[4] = {"off", "L", "R", "L+R"};
        const char *path = pcm8_pdx_path();
        printf("# adpcm   : %s\n", pcm8_enabled() ? "on" : "off");
        printf("# pdxpath : %s\n", path[0] != '\0' ? path : "(none)");
        printf("# active  : %u ch  mask %02x  pan %s\n",
               (unsigned)pcm8_active_count(), (unsigned)pcm8_active_mask(),
               PAN_NAME[pcm8_pan() & 3u]);
        printf("# keyon   : %u   miss %u\n", (unsigned)pcm8_keyon_count(),
               (unsigned)pcm8_miss_count());
        printf("# reads   : %u   clip %llu\n", (unsigned)pcm8_read_count(),
               (unsigned long long)stats_pcm_clip());
        reply_ok();
        return;
    }

    reply_err("unknown command");
}

/* ---- autoplay ------------------------------------------------------- */

/*
 * autoplay status | list | start | stop | next | prev
 *          | mode <list|random> | loop <n> | fade <ms> | gap <ms>
 *          | source <vgm|mdx|both>
 */
/* `autoplay`（引数なし）と `autoplay status` の出力。 */
static void print_autoplay_status(void)
{
    printf("# autoplay: %s  mode %s  source %s\n", autoplay_state_name(),
           autoplay_mode_name(), autoplay_source_name());

    const char *name = autoplay_current_name();
    if (name[0] != '\0')
    {
        printf("# track   : %u/%u  %s %s\n", (unsigned)autoplay_position(),
               (unsigned)autoplay_count(), autoplay_current_is_vgm() ? "vgm" : "mdx", name);
    }
    else
    {
        printf("# track   : -/%u\n", (unsigned)autoplay_count());
    }

    /* loop 0 は無限。数値のままだと「0 周でフェード」と読めるので語で出す。 */
    if (autoplay_loop() == 0u)
    {
        printf("# timing  : loop endless  fade %u ms  gap %u ms\n",
               (unsigned)autoplay_fade_ms(), (unsigned)autoplay_gap_ms());
    }
    else
    {
        printf("# timing  : loop %u  fade %u ms  gap %u ms\n", (unsigned)autoplay_loop(),
               (unsigned)autoplay_fade_ms(), (unsigned)autoplay_gap_ms());
    }
    reply_ok();
}

/* エラーなら ERR、成功なら変更後の状態を出す（`storage host` などと同じ扱い）。 */
static void autoplay_reply(const char *err)
{
    if (err != NULL)
    {
        reply_err(err);
    }
    else
    {
        print_autoplay_status();
    }
}

static void cmd_autoplay(char **cursor)
{
    char *sub = next_token(cursor);
    if (sub == NULL)
    {
        print_autoplay_status();
        return;
    }

    if (tok_is(sub, "status"))
    {
        if (expect_no_args(cursor))
        {
            print_autoplay_status();
        }
        return;
    }

    if (tok_is(sub, "list"))
    {
        if (!expect_no_args(cursor))
        {
            return;
        }
        /* 行数が多いと printf の待ちが積もるので、1 行ごとにサービスを回す。 */
        const char *err = autoplay_print_list(service_all);
        if (err != NULL)
        {
            reply_err(err);
        }
        else
        {
            reply_ok();
        }
        return;
    }

    if (tok_is(sub, "start"))
    {
        if (!expect_no_args(cursor))
        {
            return;
        }
        autoplay_reply(autoplay_start());
        return;
    }

    if (tok_is(sub, "stop"))
    {
        if (!expect_no_args(cursor))
        {
            return;
        }
        autoplay_reply(autoplay_stop());
        return;
    }

    if (tok_is(sub, "next") || tok_is(sub, "prev"))
    {
        bool forward = tok_is(sub, "next");
        if (!expect_no_args(cursor))
        {
            return;
        }
        autoplay_reply(autoplay_skip(forward ? 1 : -1));
        return;
    }

    if (tok_is(sub, "mode"))
    {
        char *arg = next_token(cursor);
        if (arg == NULL)
        {
            reply_err("wrong arity");
            return;
        }
        autoplay_mode_t mode;
        if (tok_is(arg, "list"))
        {
            mode = AUTOPLAY_MODE_LIST;
        }
        else if (tok_is(arg, "random"))
        {
            mode = AUTOPLAY_MODE_RANDOM;
        }
        else
        {
            reply_err("bad argument");
            return;
        }
        if (!expect_no_args(cursor))
        {
            return;
        }
        autoplay_set_mode(mode);
        print_autoplay_status();
        return;
    }

    if (tok_is(sub, "source"))
    {
        char *arg = next_token(cursor);
        if (arg == NULL)
        {
            reply_err("wrong arity");
            return;
        }
        autoplay_source_t source;
        if (tok_is(arg, "vgm"))
        {
            source = AUTOPLAY_SOURCE_VGM;
        }
        else if (tok_is(arg, "mdx"))
        {
            source = AUTOPLAY_SOURCE_MDX;
        }
        else if (tok_is(arg, "both"))
        {
            source = AUTOPLAY_SOURCE_BOTH;
        }
        else
        {
            reply_err("bad argument");
            return;
        }
        if (!expect_no_args(cursor))
        {
            return;
        }
        autoplay_set_source(source);
        printf("# hint    : run autoplay start to rebuild the playlist\n");
        print_autoplay_status();
        return;
    }

    /* 数値を取る 3 つ。基数は 10 進（`w` 以外は全部そう）。 */
    bool is_loop = tok_is(sub, "loop");
    bool is_fade = tok_is(sub, "fade");
    bool is_gap = tok_is(sub, "gap");
    if (is_loop || is_fade || is_gap)
    {
        char *arg = next_token(cursor);
        if (arg == NULL)
        {
            reply_err("wrong arity");
            return;
        }
        uint32_t v = 0;
        const char *err =
            parse_dec_u32(arg, is_loop ? AUTOPLAY_LOOP_MAX : AUTOPLAY_MS_MAX, &v);
        if (err != NULL)
        {
            reply_err(err);
            return;
        }
        if (!expect_no_args(cursor))
        {
            return;
        }
        if (is_loop)
        {
            autoplay_set_loop(v);
        }
        else if (is_fade)
        {
            autoplay_set_fade_ms(v);
        }
        else
        {
            autoplay_set_gap_ms(v);
        }
        print_autoplay_status();
        return;
    }

    reply_err("unknown command");
}

/* ---- ボタン操作 ------------------------------------------------------- */

/*
 * ボタン起点の操作。
 *
 * 出すのは `#` で始まる情報行だけで、OK / ERR は出さない。コマンドの応答では
 * ないので、「1 コマンド 1 応答」（README §3.3）を崩さないため。失敗の理由は
 * 各 API の戻り値をそのまま埋める。直前に各モジュールの `# hint` / `# warn` が
 * 出るので、原因は 2 行セットで読める。
 */

/* 通知行に出すボタンの名前 */
static const char *button_name(uint32_t mask)
{
    if (mask == BUTTON_MASK_BOTH)
    {
        return "SW1+SW2";
    }
    if (mask == BUTTON_MASK_SW2)
    {
        return "SW2";
    }
    return "SW1";
}

/*
 * autoplay を指定の曲順で始め直す。storage host 中なら先に取り戻す。
 * 成功なら NULL、失敗ならエラー理由。
 */
static const char *button_start_autoplay(autoplay_mode_t mode)
{
    if (storage_mode() == STORAGE_MODE_HOST)
    {
        const char *err = storage_set_player();
        if (err != NULL)
        {
            return err;
        }
        printf("# storage : %s\n", storage_mode_name());
    }

    autoplay_set_mode(mode);

    /*
     * autoplay_start() はプレイリストを作り直して 1 曲目から始める。走っていた
     * 再生（手動の vgm play / mdx play を含む）はこの中で止まるので、ここで
     * 明示的に止める必要は無い。
     */
    return autoplay_start();
}

/*
 * ファイルシステムを PC へ渡す。
 *
 * autoplay / VGM / MDX は先に止める。キャプチャは止めない（`p 1` 中は必ず
 * ホストが CDC を握っていて `p 0` を打てるので、ボタンで黙って WAV を切る
 * 利益が無い）。残る拒否要因はキャプチャ中だけになり、storage_set_host() の
 * hint がそのまま理由になる。
 */
static const char *button_enter_host(void)
{
    autoplay_stop();
    if (vgm_is_playing())
    {
        vgm_stop();
    }
    if (mdx_is_playing())
    {
        mdx_stop();
    }
    return storage_set_host();
}

/* 長押し。元のモードを破棄して新しいモードへ移る。 */
static void button_do_long(uint32_t mask)
{
    const char *err;

    if (mask == BUTTON_MASK_BOTH)
    {
        printf("# button  : SW1+SW2 long: storage host\n");
        err = button_enter_host();
        if (err != NULL)
        {
            printf("# button  : storage host failed (%s)\n", err);
        }
        return;
    }

    bool shuffle = (mask == BUTTON_MASK_SW2);

    printf("# button  : %s long: autoplay %s\n", button_name(mask), shuffle ? "random" : "list");
    err = button_start_autoplay(shuffle ? AUTOPLAY_MODE_RANDOM : AUTOPLAY_MODE_LIST);
    if (err != NULL)
    {
        printf("# button  : autoplay start failed (%s)\n", err);
    }
}

/* 短押し。曲送り。停止中はそのボタンの曲順で始める。 */
static void button_do_short(uint32_t mask)
{
    const char *err;

    if (mask == BUTTON_MASK_BOTH)
    {
        /* storage host は破壊的なので、短押しでは絶対に起こさない */
        printf("# button  : SW1+SW2 short: ignored (hold 1 s for storage host)\n");
        return;
    }

    /*
     * HOST 中は短押しを効かせない。PC がマウントしたままディスクを引き抜くと
     * 書きかけのファイルが壊れるうえ、macOS では一度 eject すると USB を挿し
     * 直すまで再マウントできない。抜けるのは長押しか SW3 のリセットだけにする。
     */
    if (storage_mode() == STORAGE_MODE_HOST)
    {
        printf("# button  : %s short: ignored (storage is handed to the PC)\n",
               button_name(mask));
        return;
    }

    bool forward = (mask != BUTTON_MASK_SW2);

    if (autoplay_is_running())
    {
        printf("# button  : %s short: %s track\n", button_name(mask),
               forward ? "next" : "prev");
        err = autoplay_skip(forward ? 1 : -1);
        if (err != NULL)
        {
            printf("# button  : autoplay %s failed (%s)\n", forward ? "next" : "prev", err);
        }
        return;
    }

    /* 停止中は曲送りが成立しないので、そのボタンの曲順で始める */
    printf("# button  : %s short: autoplay %s\n", button_name(mask),
           forward ? "list" : "random");
    err = button_start_autoplay(forward ? AUTOPLAY_MODE_LIST : AUTOPLAY_MODE_RANDOM);
    if (err != NULL)
    {
        printf("# button  : autoplay start failed (%s)\n", err);
    }
}

/*
 * ボタンのイベントを 1 個消化する。
 *
 * **必ず main() の for(;;) 直下から呼ぶ。** service_all() の中から呼ぶと、
 * コマンド処理中の待ち（`d` の遅延、`p 0` のドレイン、一覧出力の tick）から
 * 再入して filelist の走査バッファを壊し、応答の途中へ別の出力が割り込む。
 *
 * 取り出しを実行より先に済ませてあるので、実行中に押されたぶんは次の周回へ回る。
 */
static void button_dispatch(void)
{
    button_event_t ev;

    if (!button_take_event(&ev))
    {
        return;
    }

    if (ev.press == BUTTON_PRESS_LONG)
    {
        button_do_long(ev.mask);
    }
    else
    {
        button_do_short(ev.mask);
    }
}

/*
 * 起動時に押されていたボタンで動作モードを決め、両方が離されてから 1 回だけ
 * 実行する。押されていなければ何もしない（従来どおりの起動）。
 *
 * autoplay_start() は storage_init() のマウントを前提にするので、初期化列の
 * 最後（autoplay_init() の後）で呼ぶこと。
 */
static void button_boot_apply(void)
{
    uint32_t chord = button_boot_chord();

    if (chord == 0u)
    {
        return;
    }

    /* 点滅の回数で選んだモードを示す。1 = list / 2 = random / 3 = storage host */
    led_boot_pattern(chord == BUTTON_MASK_SW1 ? 1u : (chord == BUTTON_MASK_SW2 ? 2u : 3u));
    button_boot_wait_release(service_all);
    led_set_state(LED_STATE_IDLE);

    const char *label;
    const char *err;

    if (chord == BUTTON_MASK_BOTH)
    {
        label = "storage host";
        err = button_enter_host();
    }
    else if (chord == BUTTON_MASK_SW2)
    {
        label = "autoplay random";
        err = button_start_autoplay(AUTOPLAY_MODE_RANDOM);
    }
    else
    {
        label = "autoplay list";
        err = button_start_autoplay(AUTOPLAY_MODE_LIST);
    }

    snprintf(s_boot_result, sizeof(s_boot_result), "%s (%s)", label,
             (err != NULL) ? err : "ok");

    /*
     * ここまでの printf はホストが CDC を開いていないと 1 本あたり最大
     * PICO_STDIO_USB_STDOUT_TIMEOUT_US (10ms) ブロックする。複数行出すと I2S の
     * 先行量 16.4ms を超え得るので、長く止まったあとの作法どおり張り直す。
     */
    capture_resync_after_blackout();
}

/* t（自己テスト）。PCM 変換を実行し、起動時の PIO ループバックの結果も出す。 */
static void cmd_selftest(void)
{
    const char *detail = NULL;
    bool pcm_ok = ym3012_pcm_selftest(&detail);

    const char *mdx_detail = NULL;
    bool mdx_ok = mdx_selftest(&mdx_detail);

    const char *pcm8_detail = NULL;
    bool pcm8_ok = pcm8_selftest(&pcm8_detail);

    printf("# pcm     : %s\n", detail);
    printf("# piotest : %s\n", ym3012_selftest_detail());
    printf("# mdx     : %s\n", mdx_detail);
    printf("# adpcm   : %s\n", pcm8_detail);

    if (pcm_ok && mdx_ok && pcm8_ok && ym3012_selftest_passed())
    {
        reply_ok();
    }
    else
    {
        reply_err("self test failed");
    }
}

/* 1 行を処理する。空行とコメント行は無応答。 */
static void process_line(char *line)
{
    char *cursor = line;

    char *cmd = next_token(&cursor);
    if (cmd == NULL || cmd[0] == '#')
    {
        return;
    }

    /* コマンドを受け付けたことを LED で示す（待機中・キャプチャ中のどちらでも） */
    led_notify_command();

    /* 複数文字のコマンド。1 文字コマンドの経路には手を入れない。 */
    if (cmd[1] != '\0')
    {
        if (tok_is(cmd, "clock"))
        {
            cmd_clock(&cursor);
        }
        else if (tok_is(cmd, "storage"))
        {
            cmd_storage(&cursor);
        }
        else if (tok_is(cmd, "vgm"))
        {
            cmd_vgm(&cursor);
        }
        else if (tok_is(cmd, "mdx"))
        {
            cmd_mdx(&cursor);
        }
        else if (tok_is(cmd, "autoplay"))
        {
            cmd_autoplay(&cursor);
        }
        else if (tok_is(cmd, "help"))
        {
            if (expect_no_args(&cursor))
            {
                print_help();
            }
        }
        else
        {
            reply_err("unknown command");
        }
        return;
    }

    switch (tolower((unsigned char)cmd[0]))
    {
    case 'w':
        cmd_write(&cursor);
        break;
    case 'r':
        if (expect_no_args(&cursor))
        {
            if (reject_while_playing())
            {
                break;
            }
            opm_reset();
            reply_ok();
        }
        break;
    case 'c':
        if (expect_no_args(&cursor))
        {
            if (reject_while_playing())
            {
                break;
            }
            opm_clear();
            reply_ok();
        }
        break;
    case 'd':
        cmd_delay(&cursor);
        break;
    case 'p':
        cmd_pcm(&cursor);
        break;
    case 's':
        cmd_stats(&cursor);
        break;
    case 't':
        if (expect_no_args(&cursor))
        {
            cmd_selftest();
        }
        break;
    case 'i':
        if (expect_no_args(&cursor))
        {
            print_info();
        }
        break;
    case 'h':
    case '?':
        if (expect_no_args(&cursor))
        {
            print_help();
        }
        break;
    default:
        reply_err("unknown command");
        break;
    }
}

/* ---- メイン ------------------------------------------------------------ */

int main(void)
{
    /*
     * φM を整数分周で作るため、stdio 初期化より前にシステムクロックを上げる。
     * ここで決まるのは起動時のプリセットだけで、以後は clockmode.c が張り替える。
     */
    set_sys_clock_khz(OPM_SYS_CLOCK_KHZ, true);

    /*
     * CDC を 2 本にした都合で TinyUSB の初期化はアプリの責務になっている。
     * stdio_usb_init() は tud_inited() を assert するので、必ず先に呼ぶ。
     */
    tusb_init();
    stdio_init_all();

    led_init();

    /*
     * ボタンの GPIO。プルアップを最も早く効かせて整定時間を稼ぐため、また
     * 押されているかの採取をここで済ませて素早く離した利用者を取りこぼさない
     * ため、初期化列の先頭に置く。GP21 / GP22 は opm_init() の
     * gpio_init_mask()（GP2-GP14）にも ym3012 にも I2S にも含まれない。
     */
    button_init();

    stats_init();
    opm_init();

    /* PIO と DMA を起動する。以後キャプチャ経路は止めない。 */
    ym3012_init();
    capture_init();

    /* ADPCM のミキサ。ym3012 の総フレーム数を起点にするので ym3012_init() の後。 */
    pcm8_init();

    /*
     * I2S 出力。φM の分周比を使うので opm_init() より後、
     * GP26-GP28 を握るのでループバック自己診断を含む ym3012_init() より後に置く。
     */
    i2s_init();

    /* φM の実行時切り替え。現在のプリセットを控えるだけなので i2s_init() の後。 */
    clockmode_init();

    /* 内蔵フラッシュ上のファイルシステム。領域を検査して PLAYER でマウントする。 */
    storage_init();
    vgm_init();
    mdx_init();
    autoplay_init();

    /*
     * 起動時にボタンが押されていたら、離されるのを待ってからその動作モードで
     * 始める。autoplay_start() が storage のマウントを前提にするので、
     * 初期化列を全部終えたここで呼ぶ。
     */
    button_boot_apply();

    bool connected = false;
    uint32_t connect_poll_us = time_us_32();

    for (;;)
    {
        service_all();

        /*
         * ボタンの消化はここだけ。service_all() の中でやると、コマンド処理中の
         * 待ちから再入して filelist の走査バッファを壊す。ここが process_line()
         * の外側であることが構文的に保証される唯一の点。
         */
        button_dispatch();

        /*
         * 接続の確認は 100ms ごとで足りる。stdio_usb_connected() は毎周回
         * 呼ぶには重く（TinyUSB の状態を 2 段見る）、バナーはこの遅れの
         * 範囲で出れば足りる。
         */
        uint32_t now_us = time_us_32();
        if ((uint32_t)(now_us - connect_poll_us) >= CONNECT_POLL_INTERVAL_US)
        {
            connect_poll_us = now_us;

            bool now_connected = stdio_usb_connected();
            if (now_connected != connected)
            {
                connected = now_connected;
                s_len = 0;
                s_overflow = false;
                if (connected)
                {
                    /* 接続前の出力は捨てられるので、検出した時点で起動バナーを出す。 */
                    print_info();
                }
            }
        }

        /*
         * 受信 FIFO が空のときに getchar_timeout_us() まで降りると、SDK 側で
         * ドライバ走査と time_us_64() の読み出しが毎周回入る。先に TinyUSB の
         * FIFO を見て、空なら降りない。読む経路そのものは stdio のままにする。
         */
        if (tud_cdc_n_available(USB_CMD_CDC_ITF) == 0u)
        {
            tight_loop_contents();
            continue;
        }

        int ch = getchar_timeout_us(0);
        if (ch < 0)
        {
            tight_loop_contents();
            continue;
        }

        if (ch == '\r' || ch == '\n')
        {
            if (s_overflow)
            {
                reply_err("too long");
            }
            else
            {
                s_line[s_len] = '\0';
                process_line(s_line);
            }
            s_len = 0;
            s_overflow = false;
        }
        else if (s_len < LINE_MAX_LEN)
        {
            s_line[s_len++] = (char)ch;
        }
        else
        {
            s_overflow = true;
        }
    }
}
