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

#include "capture.h"
#include "flash_disk.h"
#include "i2s.h"
#include "led.h"
#include "opm.h"
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

static char s_line[LINE_MAX_LEN + 1];
static size_t s_len;
static bool s_overflow;

/* ---- メインループの 1 周分 --------------------------------------------- */

/*
 * USB・キャプチャ・I2S・LED・統計をひと回しする。
 *
 * コマンドの待ち時間（`d` の遅延や `p 0` のドレイン待ち）の中からも呼ぶので、
 * 待っている間も PCM の送出・I2S への供給・USB の処理が止まらない。
 *
 * CPU 使用率は「PCM を実際に動かせた周回」だけを busy として積む。USB の空き待ちで
 * 何も送れなかった周回は idle として扱い、送信待ちを CPU 処理時間と取り違えない。
 */
static void service_all(void) {
    uint32_t t0 = time_us_32();

    tud_task();
    bool worked = capture_service();
    worked |= i2s_service();
    worked |= vgm_service();
    worked |= storage_service();
    led_service();

    if (worked) {
        stats_busy_add(time_us_32() - t0);
    }
    stats_service();
}

/* ---- 応答 -------------------------------------------------------------- */

static void reply_ok(void) {
    puts("OK");
}

static void reply_err(const char *reason) {
    printf("ERR %s\n", reason);
}

static void print_info(void) {
    printf("# pico-opm-writer %s\n", OPM_WRITER_VERSION);
    printf("# sys_clk : %u Hz\n", (unsigned)clock_get_hz(clk_sys));
    printf("# phiM    : %u Hz (clkdiv %u + %u/256)\n",
           (unsigned)opm_clock_hz_actual(),
           (unsigned)opm_clock_div_int(),
           (unsigned)opm_clock_div_frac());
    printf("# pins    : D0-D7=GP%d-GP%d A0=GP%d /CS=GP%d /WR=GP%d /RD=GP%d /IC=GP%d\n",
           OPM_PIN_D0, OPM_PIN_D0 + 7, OPM_PIN_A0, OPM_PIN_CS, OPM_PIN_WR, OPM_PIN_RD, OPM_PIN_IC);
    printf("# pins    : phiM=GP%d /IRQ=GP%d\n",
           OPM_PIN_PHIM, OPM_PIN_IRQ);
    printf("# timing  : t_wr=%dus t_addr=%dus t_data=%dus\n",
           OPM_T_WR_US, OPM_T_ADDR_US, OPM_T_DATA_US);
    printf("# ym3012  : SO=GP%d phi1=GP%d SH1=GP%d SH2=GP%d\n",
           YM3012_PIN_SO, YM3012_PIN_PHI1, YM3012_PIN_SH1, YM3012_PIN_SH2);
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
    printf("# selftest: pio %s\n", ym3012_selftest_detail());
    printf("# storage : flash 0x%06x + %u KiB  cluster %u B  sector %u B\n",
           (unsigned)storage_region_offset(),
           (unsigned)(storage_region_size() / 1024u),
           (unsigned)FLASH_DISK_ES, (unsigned)FLASH_DISK_SS);
    printf("# vgm     : dir %s  rate %u Hz  budget %u us\n",
           VGM_DIR, (unsigned)VGM_SAMPLE_RATE, (unsigned)VGM_BUDGET_US);
    reply_ok();
}

/* `s` の出力。単位はすべてバイト。 */
static void print_stats(void) {
    uint32_t ring = stats_ring_frames() * 4u;
    uint32_t ring_max = stats_ring_frames_max() * 4u;
    uint32_t tx_cap = usb_pcm_capacity();

    printf("# state   : %s\n", capture_state_name());
    printf("# CPU     : %u%% (max %u%%)\n",
           (unsigned)stats_cpu_percent(), (unsigned)stats_cpu_percent_max());
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
    printf("# FRAMES  : %llu\n", (unsigned long long)stats_frames());
    printf("# FLASH   : WRITE %u   BLACKOUT max %u us\n",
           (unsigned)stats_flash_write(), (unsigned)stats_flash_blackout_max_us());
    const char *vgm_name = vgm_current_name();
    printf("# VGM     : %s%s%s\n", vgm_state_name(), vgm_name[0] ? " " : "", vgm_name);
    printf("# VGM POS : %llu/%u samples  loop %u\n",
           (unsigned long long)vgm_position_samples(), (unsigned)vgm_total_samples(),
           (unsigned)vgm_loop_count());
    printf("# VGM LAG : max %u us  reslip %u\n",
           (unsigned)stats_vgm_lag_max_us(), (unsigned)vgm_reslip_count());
    printf("# PIOTEST : %s\n", ym3012_selftest_detail());
    printf("# IRQ     : %s\n", opm_irq_level() ? "H" : "L");
    reply_ok();
}

static void print_help(void) {
    puts("# w <addr> <data> [<addr> <data> ...] : write register(s), hex 00-ff");
    puts("# r                                   : hardware reset (/IC)");
    puts("# c                                   : clear all registers (software)");
    puts("# d <ms>                              : delay, decimal 0-60000");
    puts("# p 1 | p 0                           : start / stop PCM output on CDC #1");
    puts("# s | s 0                             : show / reset statistics");
    puts("# t                                   : run PCM conversion self test");
    puts("# i                                   : show info");
    puts("# storage status                      : show storage state");
    puts("# storage host | storage player       : hand the flash to PC / to firmware");
    puts("# storage format yes                  : make a new filesystem (FAT12)");
    puts("# vgm list                            : list /VGM/*.vgm");
    puts("# vgm play <filename>                 : play /VGM/<filename>");
    puts("# vgm stop                            : stop playback");
    puts("# h | ?                               : show this help");
    reply_ok();
}

/* ---- トークン分割 ------------------------------------------------------ */

/* 空白 / タブ区切りで次のトークンを切り出す。無ければ NULL。 */
static char *next_token(char **cursor) {
    char *p = *cursor;

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        *cursor = p;
        return NULL;
    }

    char *start = p;
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        p++;
    }
    if (*p != '\0') {
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
static char *rest_of_line(char **cursor) {
    char *p = *cursor;

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        *cursor = p;
        return NULL;
    }

    char *start = p;
    char *end = p + strlen(p);
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
        end--;
    }
    *end = '\0';

    *cursor = end;
    return start;
}

/* トークンの大小無視比較。strcasecmp に依存しない。 */
static bool tok_is(const char *tok, const char *name) {
    for (;;) {
        int a = tolower((unsigned char)*tok);
        int b = tolower((unsigned char)*name);
        if (a != b) {
            return false;
        }
        if (a == '\0') {
            return true;
        }
        tok++;
        name++;
    }
}

/* ---- 引数のパース ------------------------------------------------------ */

/* エラー時はエラー理由の文字列、成功時は NULL を返す。 */

static const char *parse_hex_u8(const char *s, uint8_t *out) {
    uint32_t v = 0;
    bool any = false;

    for (const char *p = s; *p != '\0'; p++) {
        int c = tolower((unsigned char)*p);
        int digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else {
            return "bad argument";
        }
        v = v * 16u + (uint32_t)digit;
        if (v > 0xffu) {
            return "out of range";
        }
        any = true;
    }
    if (!any) {
        return "bad argument";
    }

    *out = (uint8_t)v;
    return NULL;
}

static const char *parse_dec_u32(const char *s, uint32_t max, uint32_t *out) {
    uint32_t v = 0;
    bool any = false;

    for (const char *p = s; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return "bad argument";
        }
        v = v * 10u + (uint32_t)(*p - '0');
        if (v > max) {
            return "out of range";
        }
        any = true;
    }
    if (!any) {
        return "bad argument";
    }

    *out = v;
    return NULL;
}

/* ---- コマンド ---------------------------------------------------------- */

/* 引数を取らないコマンド用。余分なトークンがあれば ERR を返して false。 */
static bool expect_no_args(char **cursor) {
    if (next_token(cursor) != NULL) {
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
static bool reject_while_playing(void) {
    if (vgm_is_playing()) {
        printf("# hint    : VGM 再生中。先に vgm stop を実行すること\n");
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
static void cmd_write(char **cursor) {
    if (reject_while_playing()) {
        return;
    }

    int pairs = 0;

    for (;;) {
        char *tok_addr = next_token(cursor);
        if (tok_addr == NULL) {
            if (pairs == 0) {
                reply_err("wrong arity");
            } else {
                reply_ok();
            }
            return;
        }

        char *tok_data = next_token(cursor);
        if (tok_data == NULL) {
            reply_err("wrong arity");
            return;
        }

        uint8_t addr, data;
        const char *err = parse_hex_u8(tok_addr, &addr);
        if (err == NULL) {
            err = parse_hex_u8(tok_data, &data);
        }
        if (err != NULL) {
            reply_err(err);
            return;
        }

        opm_write(addr, data);
        pairs++;
    }
}

static void cmd_delay(char **cursor) {
    char *tok = next_token(cursor);
    if (tok == NULL) {
        reply_err("wrong arity");
        return;
    }
    if (!expect_no_args(cursor)) {
        return;
    }

    uint32_t ms;
    const char *err = parse_dec_u32(tok, DELAY_MAX_MS, &ms);
    if (err != NULL) {
        reply_err(err);
        return;
    }

    /*
     * sleep_ms() で止めるとキャプチャ中に DMA リングが溢れる（16KB = 65.5ms 分しかない）。
     * 待っている間もサービスを回す。
     */
    absolute_time_t deadline = make_timeout_time_ms(ms);
    while (!time_reached(deadline)) {
        service_all();
    }
    reply_ok();
}

/*
 * p 1 / p 0
 *
 * PIO と DMA は常時動いているので、ここで制御しているのは CDC #1 への送信だけ。
 */
static void cmd_pcm(char **cursor) {
    char *tok = next_token(cursor);
    if (tok == NULL) {
        reply_err("wrong arity");
        return;
    }
    if (!expect_no_args(cursor)) {
        return;
    }

    uint32_t mode;
    const char *err = parse_dec_u32(tok, 1u, &mode);
    if (err != NULL) {
        reply_err(err);
        return;
    }

    if (mode == 1u) {
        /*
         * HOST モード中はフラッシュの消去でメインループが数十 ms 止まるので、
         * キャプチャを走らせない。判定はここに置き、capture.c に storage への
         * 依存を持ち込まない。
         */
        if (storage_mode() == STORAGE_MODE_HOST) {
            printf("# hint    : storage host 中は PCM キャプチャできない。storage player に戻すこと\n");
            reply_err("wrong state");
            return;
        }
        err = capture_start();
        if (err != NULL) {
            reply_err(err);
        } else {
            reply_ok();
        }
        return;
    }

    err = capture_request_stop();
    if (err != NULL) {
        reply_err(err);
        return;
    }

    /*
     * p 0 時点までの残りを送り切るまで待つ。待っている間も service_all() を回すので
     * DMA も USB も止まらない。ホストが CDC #1 を読まない場合に備えて上限を設ける。
     */
    absolute_time_t deadline = make_timeout_time_ms(DRAIN_TIMEOUT_MS);
    while (capture_state() == CAPTURE_STATE_DRAINING) {
        service_all();
        if (time_reached(deadline)) {
            capture_abort();
            reply_err("drain timeout");
            return;
        }
    }
    reply_ok();
}

/* s（統計表示） / s 0（統計リセット） */
static void cmd_stats(char **cursor) {
    char *tok = next_token(cursor);
    if (tok == NULL) {
        print_stats();
        return;
    }
    if (!expect_no_args(cursor)) {
        return;
    }

    uint32_t v;
    const char *err = parse_dec_u32(tok, 0u, &v);
    if (err != NULL) {
        reply_err(err);
        return;
    }

    stats_reset();
    reply_ok();
}

/* ---- storage -------------------------------------------------------- */

static void print_storage_status(void) {
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
    if (storage_space_kib(&free_kib, &total_kib)) {
        printf("# fs      : %s  cluster %u B  free %u/%u KiB\n",
               storage_fs_type_name(), (unsigned)storage_cluster_bytes(),
               (unsigned)free_kib, (unsigned)total_kib);
    } else {
        printf("# fs      : %s\n", storage_fs_state_name());
    }

    printf("# label   : %s\n", storage_label()[0] ? storage_label() : "-");
    printf("# cache   : %u lines  dirty %u\n",
           (unsigned)FLASH_DISK_CACHE_LINES, (unsigned)flash_disk_dirty_lines());
    printf("# flash   : WRITE %u   BLACKOUT max %u us\n",
           (unsigned)stats_flash_write(), (unsigned)stats_flash_blackout_max_us());
    reply_ok();
}

/*
 * storage status | host | player | format yes
 *
 * format は確認トークンを必須にする。すでにマウントできている領域を消すときは
 * format force yes を要求する。
 */
static void cmd_storage(char **cursor) {
    char *sub = next_token(cursor);
    if (sub == NULL) {
        reply_err("wrong arity");
        return;
    }

    if (tok_is(sub, "status")) {
        if (expect_no_args(cursor)) {
            print_storage_status();
        }
        return;
    }

    if (tok_is(sub, "host")) {
        if (!expect_no_args(cursor)) {
            return;
        }
        const char *err = storage_set_host();
        if (err != NULL) {
            reply_err(err);
        } else {
            reply_ok();
        }
        return;
    }

    if (tok_is(sub, "player")) {
        if (!expect_no_args(cursor)) {
            return;
        }
        const char *err = storage_set_player();
        if (err != NULL) {
            reply_err(err);
        } else {
            reply_ok();
        }
        return;
    }

    if (tok_is(sub, "format")) {
        char *tok = next_token(cursor);
        bool force = false;
        if (tok != NULL && tok_is(tok, "force")) {
            force = true;
            tok = next_token(cursor);
        }
        if (tok == NULL || !tok_is(tok, "yes")) {
            reply_err("bad argument");
            return;
        }
        if (!expect_no_args(cursor)) {
            return;
        }
        if (storage_mode() != STORAGE_MODE_PLAYER) {
            printf("# hint    : storage player に戻してからフォーマットすること\n");
            reply_err("wrong state");
            return;
        }
        if (!force && storage_fs_state() == STORAGE_FS_MOUNTED) {
            printf("# hint    : すでにファイルシステムがある。消すなら storage format force yes\n");
            reply_err("wrong state");
            return;
        }

        /* ガードを全部抜けてから進捗を出す */
        printf("# format  : 数十秒かかる。この間 I2S はアンダーランする\n");
        const char *err = storage_format();
        if (err != NULL) {
            reply_err(err);
        } else {
            reply_ok();
        }
        return;
    }

    reply_err("unknown command");
}

/* ---- vgm ------------------------------------------------------------ */

/*
 * vgm list | play <filename> | stop
 *
 * ファイル名は行の残り全部を 1 引数として受けるので、空白を含む名前も扱える。
 */
static void cmd_vgm(char **cursor) {
    char *sub = next_token(cursor);
    if (sub == NULL) {
        reply_err("wrong arity");
        return;
    }

    if (tok_is(sub, "list")) {
        if (!expect_no_args(cursor)) {
            return;
        }
        /* 行数が多いと printf の待ちが積もるので、1 行ごとにサービスを回す。 */
        const char *err = vgm_list(service_all);
        if (err != NULL) {
            reply_err(err);
        } else {
            reply_ok();
        }
        return;
    }

    if (tok_is(sub, "play")) {
        char *name = rest_of_line(cursor);
        if (name == NULL) {
            reply_err("wrong arity");
            return;
        }
        const char *err = vgm_play(name);
        if (err != NULL) {
            reply_err(err);
        } else {
            reply_ok();
        }
        return;
    }

    if (tok_is(sub, "stop")) {
        if (!expect_no_args(cursor)) {
            return;
        }
        const char *err = vgm_stop();
        if (err != NULL) {
            reply_err(err);
        } else {
            reply_ok();
        }
        return;
    }

    reply_err("unknown command");
}

/* t（自己テスト）。PCM 変換を実行し、起動時の PIO ループバックの結果も出す。 */
static void cmd_selftest(void) {
    const char *detail = NULL;
    bool pcm_ok = ym3012_pcm_selftest(&detail);

    printf("# pcm     : %s\n", detail);
    printf("# pio     : %s\n", ym3012_selftest_detail());

    if (pcm_ok && ym3012_selftest_passed()) {
        reply_ok();
    } else {
        reply_err("self test failed");
    }
}

/* 1 行を処理する。空行とコメント行は無応答。 */
static void process_line(char *line) {
    char *cursor = line;

    char *cmd = next_token(&cursor);
    if (cmd == NULL || cmd[0] == '#') {
        return;
    }

    /* コマンドを受け付けたことを LED で示す（待機中・キャプチャ中のどちらでも） */
    led_notify_command();

    /* 複数文字のコマンド。1 文字コマンドの経路には手を入れない。 */
    if (cmd[1] != '\0') {
        if (tok_is(cmd, "storage")) {
            cmd_storage(&cursor);
        } else if (tok_is(cmd, "vgm")) {
            cmd_vgm(&cursor);
        } else {
            reply_err("unknown command");
        }
        return;
    }

    switch (tolower((unsigned char)cmd[0])) {
    case 'w':
        cmd_write(&cursor);
        break;
    case 'r':
        if (expect_no_args(&cursor)) {
            if (reject_while_playing()) {
                break;
            }
            opm_reset();
            reply_ok();
        }
        break;
    case 'c':
        if (expect_no_args(&cursor)) {
            if (reject_while_playing()) {
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
        if (expect_no_args(&cursor)) {
            cmd_selftest();
        }
        break;
    case 'i':
        if (expect_no_args(&cursor)) {
            print_info();
        }
        break;
    case 'h':
    case '?':
        if (expect_no_args(&cursor)) {
            print_help();
        }
        break;
    default:
        reply_err("unknown command");
        break;
    }
}

/* ---- メイン ------------------------------------------------------------ */

int main(void) {
    /* φM を整数分周で作るため、stdio 初期化より前にシステムクロックを上げる。 */
    set_sys_clock_khz(OPM_SYS_CLOCK_KHZ, true);

    /*
     * CDC を 2 本にした都合で TinyUSB の初期化はアプリの責務になっている。
     * stdio_usb_init() は tud_inited() を assert するので、必ず先に呼ぶ。
     */
    tusb_init();
    stdio_init_all();

    led_init();
    stats_init();
    opm_init();

    /* PIO と DMA を起動する。以後キャプチャ経路は止めない。 */
    ym3012_init();
    capture_init();

    /*
     * I2S 出力。φM の分周比を使うので opm_init() より後、
     * GP26-GP28 を握るのでループバック自己診断を含む ym3012_init() より後に置く。
     */
    i2s_init();

    /* 内蔵フラッシュ後半のファイルシステム。領域を検査して PLAYER でマウントする。 */
    storage_init();
    vgm_init();

    bool connected = false;

    for (;;) {
        service_all();

        bool now_connected = stdio_usb_connected();
        if (now_connected != connected) {
            connected = now_connected;
            s_len = 0;
            s_overflow = false;
            if (connected) {
                /* 接続前の出力は捨てられるので、検出した時点で起動バナーを出す。 */
                print_info();
            }
        }

        int ch = getchar_timeout_us(0);
        if (ch < 0) {
            tight_loop_contents();
            continue;
        }

        if (ch == '\r' || ch == '\n') {
            if (s_overflow) {
                reply_err("too long");
            } else {
                s_line[s_len] = '\0';
                process_line(s_line);
            }
            s_len = 0;
            s_overflow = false;
        } else if (s_len < LINE_MAX_LEN) {
            s_line[s_len++] = (char)ch;
        } else {
            s_overflow = true;
        }
    }
}
