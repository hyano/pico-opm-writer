/*
 * pico-opm-writer
 *
 * USB CDC のテキストコマンドから YM2151 (OPM) のレジスタを書き込む。
 *
 * SPDX-License-Identifier: MIT
 */
#include <ctype.h>
#include <stdio.h>

#include "hardware/clocks.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "capture.h"
#include "led.h"
#include "opm.h"
#include "stats.h"
#include "usb_pcm.h"
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
 * USB・キャプチャ・LED・統計をひと回しする。
 *
 * コマンドの待ち時間（`d` の遅延や `p 0` のドレイン待ち）の中からも呼ぶので、
 * 待っている間も PCM の送出と USB の処理が止まらない。
 *
 * CPU 使用率は「PCM を実際に動かせた周回」だけを busy として積む。USB の空き待ちで
 * 何も送れなかった周回は idle として扱い、送信待ちを CPU 処理時間と取り違えない。
 */
static void service_all(void) {
    uint32_t t0 = time_us_32();

    tud_task();
    bool worked = capture_service();
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
    printf("# selftest: pio %s\n", ym3012_selftest_detail());
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
    printf("# OVERRUN : %u   E0 : %llu   RXSTALL : %u\n",
           (unsigned)stats_overrun(), (unsigned long long)stats_forbidden(),
           (unsigned)stats_rxstall());
    printf("# RATE    : %u frames/s (expect %u)\n",
           (unsigned)stats_frame_rate(), (unsigned)(opm_clock_hz_actual() / 64u));
    printf("# FRAMES  : %llu\n", (unsigned long long)stats_frames());
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
 * w <addr> <data> [<addr> <data> ...]
 *
 * ペアを読むそばから書き込むため、途中でエラーになってもそこまでの書き込みは
 * 実行済みのまま ERR を返す（仕様どおり）。
 */
static void cmd_write(char **cursor) {
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

    if (cmd[1] != '\0') {
        reply_err("unknown command");
        return;
    }

    switch (tolower((unsigned char)cmd[0])) {
    case 'w':
        cmd_write(&cursor);
        break;
    case 'r':
        if (expect_no_args(&cursor)) {
            opm_reset();
            reply_ok();
        }
        break;
    case 'c':
        if (expect_no_args(&cursor)) {
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
