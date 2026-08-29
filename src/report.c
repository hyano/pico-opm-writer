/*
 * `i` / `s` / `h` の状態表示の実装
 *
 * ここが出すのは本文だけで、末尾の OK は出さない。「1 コマンド 1 応答」
 * （README §3.3「応答」）を守る責任はコマンドの受け口（console.c）に置いてある。
 *
 * SPDX-License-Identifier: MIT
 */
#include "report.h"

#include <stdio.h>

#include "hardware/clocks.h"

#include "button.h"
#include "buttonmap.h"
#include "capture.h"
#include "clockmode.h"
#include "flash_disk.h"
#include "i2s.h"
#include "mdx.h"
#include "opm.h"
#include "pcm8.h"
#include "stats.h"
#include "storage.h"
#include "usb_pcm.h"
#include "vgm.h"
#include "ym3012.h"

void report_info(void)
{
    /* OPM_WRITER_VERSION は CMakeLists.txt の project(VERSION) から -D で渡る */
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
    /* t_data だけは φM から実行時に算出しているので、実効値を出す */
    uint32_t data_ns = opm_data_wait_ns();
    printf("# timing  : t_wr=%dus t_addr=%dus t_data=%u.%uus (%u phiM) t_rd=%dus\n",
           OPM_T_WR_US, OPM_T_ADDR_US,
           (unsigned)(data_ns / 1000u), (unsigned)((data_ns % 1000u) / 100u),
           (unsigned)OPM_BUSY_CYCLES, OPM_T_RD_US);
    printf("# ym3012  : SO=GP%d phi1=GP%d SH1=GP%d SH2=GP%d\n",
           YM3012_PIN_SO, YM3012_PIN_PHI1, YM3012_PIN_SH1, YM3012_PIN_SH2);
#if BUTTON_ENABLED
    printf("# pins    : SW1=GP%u SW2=GP%u (pull-up, active low)\n",
           (unsigned)BUTTON_PIN_SW1, (unsigned)BUTTON_PIN_SW2);
    printf("# button  : long %u ms  boot %s\n",
           (unsigned)(BUTTON_LONG_PRESS_US / 1000u), buttonmap_boot_result());
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

    /*
     * OPM バス。avg が 1 レジスタ書き込みの実費用そのものなので、タイミング定数を
     * 変えたときの前後比較はここを見る。avg は 1/100us まで出す。
     */
    uint32_t writes = stats_opm_writes();
    uint32_t avg100 = (writes != 0u)
                          ? (uint32_t)(((uint64_t)stats_opm_write_us() * 100u) / writes)
                          : 0u;
    printf("# OPMW    : %u writes/s  %u us/s  avg %u.%02u us  max %u us\n",
           (unsigned)writes, (unsigned)stats_opm_write_us(),
           (unsigned)(avg100 / 100u), (unsigned)(avg100 % 100u),
           (unsigned)stats_opm_write_max_us());
#endif
}

/* `s` の出力。単位はすべてバイト。 */
void report_stats(void)
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
    /* 無音が続いているフレーム数。曲の終わりの余韻を待つ判定と同じ値。 */
    printf("# QUIET   : %llu frames\n",
           (unsigned long long)(ym3012_write_total() - ym3012_last_loud_total()));
    /*
     * LOOP の MAX と AUDIO GAP がリアルタイム余裕の直接の指標。I2S の MIN は
     * 余裕が削られた結果、SEQ LAG はその症状で、原因はこちらに出る。
     */
    printf("# LOOP    : %u passes/s  MAX %u us  AUDIO GAP max %u us\n",
           (unsigned)stats_loop_rate(), (unsigned)stats_loop_period_max_us(),
           (unsigned)stats_audio_gap_max_us());
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
}

void report_help(void)
{
    puts("# w <addr> <data> [<addr> <data> ...] : write register(s), hex 00-ff");
    puts("# r 0 | r 1                           : read one byte with A0=0 / A0=1");
    puts("# reset                               : hardware reset (/IC)");
    puts("# c                                   : clear all registers (software)");
    puts("# d <ms>                              : delay, decimal 0-60000");
    puts("# p | p 1 | p 0                       : show / start / stop PCM output on CDC #1");
    puts("# p 2                                 : start on the next play, stop when the song ends");
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
    puts("# vgm loop | vgm loop <n>             : show / set loops before fade-out, 0 = endless");
    puts("# vgm fade | vgm fade <ms>            : show / set fade-out length, 0 = stop at once");
    puts("# mdx | mdx status                    : show MDX playback state");
    puts("# mdx list                            : list /MDX/**/*.mdx");
    puts("# mdx play <path>                     : play /MDX/<path>, subfolders ok");
    puts("# mdx stop                            : stop playback");
    puts("# mdx loop | mdx loop <n>             : show / set loops before fade-out, 0 = endless");
    puts("# mdx fade | mdx fade <ms>            : show / set fade-out length, 0 = stop at once");
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
}
