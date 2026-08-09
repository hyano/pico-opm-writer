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

#include "opm.h"

/* 1 行の最大長（これを超えたら ERR too long） */
#define LINE_MAX_LEN 255

/* `d` コマンドの上限 */
#define DELAY_MAX_MS 60000u

static char s_line[LINE_MAX_LEN + 1];
static size_t s_len;
static bool s_overflow;

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
    printf("# pins    : D0-D7=GP%d-GP%d A0=GP%d /WR=GP%d /IC=GP%d phiM=GP%d\n",
           OPM_PIN_D0, OPM_PIN_D0 + 7, OPM_PIN_A0, OPM_PIN_WR, OPM_PIN_IC, OPM_PIN_PHIM);
    printf("# timing  : t_wr=%dus t_addr=%dus t_data=%dus\n",
           OPM_T_WR_US, OPM_T_ADDR_US, OPM_T_DATA_US);
    reply_ok();
}

static void print_help(void) {
    puts("# w <addr> <data> [<addr> <data> ...] : write register(s), hex 00-ff");
    puts("# r                                   : hardware reset (/IC)");
    puts("# c                                   : clear all registers (software)");
    puts("# d <ms>                              : delay, decimal 0-60000");
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

    sleep_ms(ms);
    reply_ok();
}

/* 1 行を処理する。空行とコメント行は無応答。 */
static void process_line(char *line) {
    char *cursor = line;

    char *cmd = next_token(&cursor);
    if (cmd == NULL || cmd[0] == '#') {
        return;
    }
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

    stdio_init_all();
    opm_init();

    bool connected = false;

    for (;;) {
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
