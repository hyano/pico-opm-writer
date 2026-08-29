/*
 * コマンドの受け口の実装
 *
 * CDC #0 から 1 文字ずつ受け取って行に組み、`process_line()` が 1 行を
 * 解釈する。**応答は 1 コマンドにつき OK か ERR がちょうど 1 行**
 * （README §3.3「応答」）で、その規約を守るのはこのファイルの責務。
 * 状態表示の本文は report.c が出す。
 *
 * SPDX-License-Identifier: MIT
 */
#include "console.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "autoplay.h"
#include "capture.h"
#include "clockmode.h"
#include "flash_disk.h"
#include "i2s.h"
#include "led.h"
#include "mdx.h"
#include "opm.h"
#include "pcm8.h"
#include "report.h"
#include "sched.h"
#include "songend.h"
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

/* 接続状態の確認間隔。起動バナーはこの遅れの範囲で出れば足りる。 */
#ifndef CONNECT_POLL_INTERVAL_US
#define CONNECT_POLL_INTERVAL_US 100000u
#endif

/* 組み立て中の行 */
static char s_line[LINE_MAX_LEN + 1];
static size_t s_len;
static bool s_overflow;

/* ---- 応答 -------------------------------------------------------------- */

void reply_ok(void)
{
    puts("OK");
}

void reply_err(const char *reason)
{
    printf("ERR %s\n", reason);
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

/*
 * r 0 / r 1
 *
 * /RD で 1 バイト読み出す。A0=1 がデータシート上のステータスレジスタ。
 * レジスタを変えないので、再生中でも拒否しない（w / reset / c とはここが違う）。
 */
static void cmd_read(char **cursor)
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

    uint32_t a0;
    const char *err = parse_dec_u32(tok, 1u, &a0);
    if (err != NULL)
    {
        reply_err(err);
        return;
    }

    printf("# read    : a0=%u data=0x%02x\n", (unsigned)a0, opm_read(a0 != 0u));
    reply_ok();
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
 * p 1 / p 2 / p 0
 *
 * PIO と DMA は常時動いているので、ここで制御しているのは CDC #1 への送信だけ。
 */
/* `p`（引数なし）の出力。キャプチャの可否をこれだけで判断できるようにする。 */
static void print_capture_status(void)
{
    printf("# capture : %s\n", capture_state_name());
    printf("# auto    : %s\n", capture_is_auto() ? "on" : "off");
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
    const char *err = parse_dec_u32(tok, 2u, &mode);
    if (err != NULL)
    {
        reply_err(err);
        return;
    }

    if (mode != 0u)
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
        err = capture_start(mode == 2u, songend_track_seq());
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
        report_stats();
        reply_ok();
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
/*
 * `vgm loop` / `vgm fade` と MDX 版の共通部。曲の終わり方の設定は種別ごとに
 * songend が持っていて、ここは表示と入力の検査だけを行う。
 */
static void print_songend_setting(songend_kind_t kind)
{
    /* loop 0 は無限。数値のままだと「0 周でフェード」と読めるので語で出す。 */
    uint32_t loops = songend_loop(kind);
    if (loops == 0u)
    {
        printf("# end     : loop endless  fade %u ms\n", (unsigned)songend_fade_ms(kind));
    }
    else
    {
        printf("# end     : loop %u  fade %u ms\n", (unsigned)loops,
               (unsigned)songend_fade_ms(kind));
    }
}

/* 引数なしなら表示だけ。あれば設定してから変更後の状態を返す。 */
static void cmd_songend_setting(char **cursor, songend_kind_t kind, bool is_loop)
{
    char *arg = next_token(cursor);
    if (arg != NULL)
    {
        if (!expect_no_args(cursor))
        {
            return;
        }
        uint32_t v;
        const char *err =
            parse_dec_u32(arg, is_loop ? SONGEND_LOOP_MAX : SONGEND_FADE_MS_MAX, &v);
        if (err != NULL)
        {
            reply_err(err);
            return;
        }
        if (is_loop)
        {
            songend_set_loop(kind, v);
        }
        else
        {
            songend_set_fade_ms(kind, v);
        }
    }
    print_songend_setting(kind);
    reply_ok();
}

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
    print_songend_setting(SONGEND_KIND_VGM);
    printf("# song    : %s\n", songend_state_name());
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

    if (tok_is(sub, "loop") || tok_is(sub, "fade"))
    {
        cmd_songend_setting(cursor, SONGEND_KIND_VGM, tok_is(sub, "loop"));
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
    print_songend_setting(SONGEND_KIND_MDX);
    printf("# song    : %s\n", songend_state_name());
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

    if (tok_is(sub, "loop") || tok_is(sub, "fade"))
    {
        cmd_songend_setting(cursor, SONGEND_KIND_MDX, tok_is(sub, "loop"));
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

    printf("# song    : %s\n", songend_state_name());

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
        else if (tok_is(cmd, "reset"))
        {
            if (expect_no_args(&cursor))
            {
                if (!reject_while_playing())
                {
                    opm_reset();
                    reply_ok();
                }
            }
        }
        else if (tok_is(cmd, "help"))
        {
            if (expect_no_args(&cursor))
            {
                report_help();
            reply_ok();
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
        cmd_read(&cursor);
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
            report_info();
            reply_ok();
        }
        break;
    case 'h':
    case '?':
        if (expect_no_args(&cursor))
        {
            report_help();
            reply_ok();
        }
        break;
    default:
        reply_err("unknown command");
        break;
    }
}

/* ---- 入力 -------------------------------------------------------------- */

void console_poll_connect(void)
{
    static bool s_connected;
    static uint32_t s_poll_us;

    /*
     * 100ms ごとで足りる。stdio_usb_connected() は毎周回呼ぶには重く
     * （TinyUSB の状態を 2 段見る）、バナーはこの遅れの範囲で出れば足りる。
     */
    uint32_t now_us = time_us_32();
    if ((uint32_t)(now_us - s_poll_us) < CONNECT_POLL_INTERVAL_US)
    {
        return;
    }
    s_poll_us = now_us;

    bool now_connected = stdio_usb_connected();
    if (now_connected == s_connected)
    {
        return;
    }
    s_connected = now_connected;

    /* 書きかけの行は接続をまたいで持ち越さない */
    s_len = 0;
    s_overflow = false;

    if (now_connected)
    {
        /* 接続前の出力は捨てられるので、検出した時点で起動バナーを出す。 */
        report_info();
        reply_ok();
    }
}

void console_service(void)
{
    /*
     * 受信 FIFO が空のときに getchar_timeout_us() まで降りると、SDK 側で
     * ドライバ走査と time_us_64() の読み出しが毎周回入る。先に TinyUSB の
     * FIFO を見て、空なら降りない。読む経路そのものは stdio のままにする。
     */
    if (tud_cdc_n_available(USB_CMD_CDC_ITF) == 0u)
    {
        tight_loop_contents();
        return;
    }

    int ch = getchar_timeout_us(0);
    if (ch < 0)
    {
        tight_loop_contents();
        return;
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
