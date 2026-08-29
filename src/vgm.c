/*
 * VGM ファイルの再生の実装
 *
 * SPDX-License-Identifier: MIT
 */
#include "vgm.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "ff.h"

#include "clockmode.h"
#include "filelist.h"
#include "mdx.h"
#include "opm.h"
#include "stats.h"
#include "storage.h"
#include "vgz.h"

/* ---- ヘッダのオフセット ------------------------------------------------ */

#define VGM_HDR_SIZE 0x40u

#define VGM_OFF_MAGIC        0x00u
#define VGM_OFF_EOF          0x04u
#define VGM_OFF_VERSION      0x08u
#define VGM_OFF_GD3          0x14u
#define VGM_OFF_TOTAL        0x18u
#define VGM_OFF_LOOP         0x1Cu
#define VGM_OFF_LOOP_SAMPLES 0x20u
#define VGM_OFF_YM2151_CLOCK 0x30u
#define VGM_OFF_DATA         0x34u

/* ---- 状態 -------------------------------------------------------------- */

static vgm_state_t s_state;

static FIL s_fp;
static bool s_open;

static char s_name[FILELIST_PATH_MAX + 1u]; /* VGM_DIR からの相対パス */

/* ヘッダから読んだもの */
static uint32_t s_version;
static uint32_t s_file_clock_hz;
static uint32_t s_total_samples;
static uint32_t s_gd3_offset; /* 将来 GD3 を読むときのために保持しておく */
static uint32_t s_data_start;
static uint32_t s_loop_target; /* 0 ならループしない */
static uint32_t s_end_bound;   /* ここに達したら終端として扱う */

/* スケジューラ */
static uint64_t s_start_us;
static uint64_t s_samples; /* 発行済みコマンドまでのサンプル数（ループを跨いで連続） */
static uint64_t s_due_us;
static uint32_t s_loops;
static uint32_t s_reslips;
static uint32_t s_play_seq; /* 再生を始めた回数。vgm_play_seq() の実体 */

/*
 * ストリーム用のバッファ。非圧縮なら 1 クラスタぶん読むので f_read は FIL の
 * 512 バイト窓を通らず disk_read(count=8) に直行し、XIP からの memcpy 1 回で済む。
 * 複数バイトのコマンドは vgm_getc() を繰り返し呼ぶだけなので、境界処理は要らない。
 *
 * .vgz のときもこの同じバッファを使い、補充だけを vgz_read() に切り替える。
 * vgm_getc() 以降のコマンド解釈は圧縮の有無を知らない。
 */
static uint8_t s_buf[4096];
static uint32_t s_buf_len;
static uint32_t s_buf_pos;
static uint32_t s_buf_base; /* s_buf[0] の（展開後の）ファイル内位置 */

/* ---- .vgz の状態 ------------------------------------------------------- */

static bool s_gz;            /* 再生中のファイルが .vgz か */
static uint32_t s_skip_left; /* 分割中の前方読み捨ての残り */
static uint32_t s_gz_reloads;

/*
 * ループ先頭を通過するときは s_buf ごと保存する。展開器の状態だけ戻しても、
 * 復元位置が s_buf の途中になってしまうため。
 */
static uint8_t s_snap_buf[sizeof(s_buf)];
static uint32_t s_snap_buf_len;
static uint32_t s_snap_buf_pos;
static uint32_t s_snap_buf_base;

/* ---- バイト列の読み出し ------------------------------------------------ */

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* 現在の（展開後の）ファイル内位置 */
static uint32_t vgm_tell(void)
{
    return s_buf_base + s_buf_pos;
}

/*
 * ファイル全体の長さ。.vgz では gzip トレーラの ISIZE を使う。
 * ISIZE が信用できないときは VGZ_SIZE_UNKNOWN が返り、上限の判定は
 * ヘッダの EOF / GD3 オフセットに委ねられる。
 */
static uint32_t vgm_size(void)
{
    return s_gz ? vgz_isize() : (uint32_t)f_size(&s_fp);
}

static bool vgm_refill(void)
{
    if (s_gz)
    {
        s_buf_base = vgz_tell();
        uint32_t n = vgz_read(s_buf, VGM_GZ_CHUNK);
        s_buf_len = n;
        s_buf_pos = 0;
        return n > 0u;
    }

    UINT n = 0;
    s_buf_base = (uint32_t)f_tell(&s_fp);
    if (f_read(&s_fp, s_buf, sizeof(s_buf), &n) != FR_OK || n == 0u)
    {
        s_buf_len = 0;
        s_buf_pos = 0;
        return false;
    }
    s_buf_len = (uint32_t)n;
    s_buf_pos = 0;
    return true;
}

/* 1 バイト読む。終端・エラーなら -1。 */
static int vgm_getc(void)
{
    if (s_buf_pos >= s_buf_len)
    {
        if (!vgm_refill())
        {
            return -1;
        }
    }
    return s_buf[s_buf_pos++];
}

/*
 * ヘッダなど、決まった長さをまとめて読む。
 * 全部読めたら true。
 */
static bool vgm_read(uint8_t *dst, uint32_t n)
{
    bool ok;
    if (s_gz)
    {
        ok = vgz_read(dst, n) == n;
        s_buf_base = vgz_tell();
    }
    else
    {
        UINT rd = 0;
        ok = f_read(&s_fp, dst, n, &rd) == FR_OK && rd == n;
        s_buf_base = (uint32_t)f_tell(&s_fp);
    }
    /* s_buf を経由しないので、vgm_tell() が合うように畳んでおく。 */
    s_buf_len = 0;
    s_buf_pos = 0;
    return ok;
}

/*
 * .vgz の前方読み捨てを予約する。展開しながら捨てるしかないので、
 * ここでは残り数を積むだけにして vgm_step() が予算内で少しずつ消化する。
 */
static void gz_skip_ahead(uint32_t n)
{
    s_buf_base = vgm_tell();
    s_buf_len = 0;
    s_buf_pos = 0;
    s_skip_left = n;
}

/* n バイト読み飛ばす */
static bool vgm_skip(uint32_t n)
{
    uint32_t in_buf = s_buf_len - s_buf_pos;
    if (n <= in_buf)
    {
        s_buf_pos += n;
        return true;
    }

    /*
     * バッファに無い分は飛ばす。0x67 のデータブロックは MB 単位になりうるので、
     * 1 バイトずつ読んで飛ばしてはいけない。
     */
    uint32_t target = vgm_tell() + n;
    if (target > vgm_size())
    {
        return false;
    }
    if (s_gz)
    {
        /* gzip は途中から読めないので、展開して捨てるしかない。 */
        s_buf_pos = s_buf_len; /* バッファに残っている分は消費済みにする */
        gz_skip_ahead(n - in_buf);
        return true;
    }
    if (f_lseek(&s_fp, target) != FR_OK)
    {
        return false;
    }
    s_buf_len = 0;
    s_buf_pos = 0;
    s_buf_base = target;
    return true;
}

static bool vgm_seek(uint32_t pos)
{
    if (pos > vgm_size())
    {
        return false;
    }

    if (s_gz)
    {
        /*
         * ループ先頭なら、保存しておいた展開器の状態へ即座に戻す。
         * 保存位置は s_buf まで含めた論理位置で見る（vgz 側は s_buf に
         * 先読みしたぶんだけ先を指している）。
         */
        if (vgz_snapshot_valid() && s_snap_buf_base + s_snap_buf_pos == pos)
        {
            if (!vgz_restore())
            {
                return false;
            }
            memcpy(s_buf, s_snap_buf, sizeof(s_buf));
            s_buf_len = s_snap_buf_len;
            s_buf_pos = s_snap_buf_pos;
            s_buf_base = s_snap_buf_base;
            return true;
        }
        /*
         * 保存が無い。後ろへ戻るなら先頭から展開し直すしかない。
         * ここに来ると継ぎ目で音が数百 ms 途切れるので、回数を数えて s で見せる。
         */
        if (pos < vgm_tell())
        {
            if (!vgz_rewind())
            {
                return false;
            }
            s_buf_len = 0;
            s_buf_pos = 0;
            s_buf_base = 0;
            s_gz_reloads++;
            printf("# warn    : could not save the .vgz loop point; will re-inflate from the start\n");
        }
        gz_skip_ahead(pos - vgm_tell());
        return true;
    }

    if (f_lseek(&s_fp, pos) != FR_OK)
    {
        return false;
    }
    s_buf_len = 0;
    s_buf_pos = 0;
    s_buf_base = pos;
    return true;
}

/* ---- コマンドのオペランド長 -------------------------------------------- */

/*
 * オペランドのバイト数。0xFF は「未知なので中断」。
 * wait や YM2151 書き込みなど、動作を伴うものは vgm_step() で個別に処理する。
 */
static uint8_t operand_len(uint8_t op)
{
    if (op <= 0x2Fu)
    {
        return 0xFFu; /* 未知 */
    }
    if (op <= 0x3Fu)
    {
        return 1u; /* 0x30-0x3F: 0x50-0x5F の 2 個目のチップ */
    }
    if (op <= 0x4Eu)
    {
        /* 0x40-0x4E は v1.60 でオペランドが 1 -> 2 に増えた */
        return (s_version >= 0x160u) ? 2u : 1u;
    }
    if (op == 0x4Fu)
    {
        return 1u; /* GG PSG ステレオ */
    }
    if (op == 0x50u)
    {
        return 1u; /* SN76489 */
    }
    if (op <= 0x5Fu)
    {
        return 2u; /* 0x51-0x5F: YM2413 / YM2612 / YM2203 ... */
    }
    if (op == 0x64u)
    {
        return 3u; /* 0x62/0x63 の長さ上書き（非推奨） */
    }
    if (op == 0x68u)
    {
        return 11u; /* PCM RAM write */
    }
    if (op >= 0x90u && op <= 0x95u)
    {
        static const uint8_t dac_stream[6] = {4u, 4u, 5u, 10u, 1u, 4u};
        return dac_stream[op - 0x90u]; /* 0x90-0x95: DAC ストリーム制御 */
    }
    if (op >= 0xA0u && op <= 0xBFu)
    {
        return 2u;
    }
    if (op >= 0xC0u && op <= 0xDFu)
    {
        return 3u;
    }
    if (op >= 0xE0u)
    {
        return 4u;
    }
    return 0xFFu; /* 0x60 / 0x65 / 0x69-0x6F / 0x96-0x9F */
}

/* ---- エラー ------------------------------------------------------------ */

/*
 * 再生中に見つかった異常は、コマンドとしては既に OK を返したあとなので
 * 非同期通知にする（capture.c が DMA overrun でやっているのと同じ）。
 */
static void vgm_fail(const char *why, uint32_t at)
{
    printf("# ERR vgm bad file (%s at 0x%08x)\n", why, (unsigned)at);
    s_state = VGM_STATE_ERROR;
}

/* ---- 停止 -------------------------------------------------------------- */

/* 手順とその理由は opm_key_off_all() の側に書いてある。 */
static void key_off_all(void)
{
    opm_key_off_all();
}

static void close_file(void)
{
    vgz_close();
    if (s_open)
    {
        f_close(&s_fp);
        s_open = false;
    }
    s_gz = false;
    s_skip_left = 0;
    s_buf_len = 0;
    s_buf_pos = 0;
    s_buf_base = 0;
}

/* ---- 1 コマンドの実行 -------------------------------------------------- */

/* サンプル数を進めて次の予定時刻を計算する */
static void advance_samples(uint32_t n)
{
    s_samples += n;
    /*
     * 絶対サンプル数から毎回計算し直すので、丸め誤差が累積しない。
     * s_samples * 1000000 が u64 を溢れるのは 13 年ぶん再生したあと。
     */
    s_due_us = s_start_us + (s_samples * 1000000ull) / VGM_SAMPLE_RATE;
}

/*
 * .vgz の前方読み捨てを 1 チャンクだけ進める。s_buf を捨て先に使う
 * （読み捨て中は s_buf に有効なデータが無い）。続行できるなら true。
 */
static bool gz_skip_chunk(void)
{
    uint32_t n = (s_skip_left > VGM_GZ_CHUNK) ? VGM_GZ_CHUNK : s_skip_left;
    uint32_t got = vgz_read(s_buf, n);
    s_skip_left -= got;
    s_buf_base = vgz_tell();
    s_buf_len = 0;
    s_buf_pos = 0;
    if (got < n)
    {
        vgm_fail("truncated", s_buf_base);
        return false;
    }
    return true;
}

/* 続行できるなら true。停止・エラーなら false。 */
static bool vgm_step(void)
{
    if (s_skip_left > 0u)
    {
        /*
         * .vgz の読み捨ての途中。vgm_service() の予算ループにそのまま乗せて、
         * 1 周回で 500us ぶんずつ進める。ここで生じた遅れは
         * VGM_RESYNC_LAG_US による時計の張り直しが吸収する。
         */
        return gz_skip_chunk();
    }

    uint32_t at = vgm_tell();

    /*
     * ループ先頭をはじめて通過する瞬間に展開器の状態を保存する。gzip は後方
     * シークできないので、これが 2 周目以降の唯一の戻り道になる。
     */
    if (s_gz && s_loop_target != 0u && at == s_loop_target && !vgz_snapshot_valid())
    {
        vgz_snapshot();
        memcpy(s_snap_buf, s_buf, sizeof(s_snap_buf));
        s_snap_buf_len = s_buf_len;
        s_snap_buf_pos = s_buf_pos;
        s_snap_buf_base = s_buf_base;
    }

    if (at >= s_end_bound)
    {
        /* GD3 や EOF の境界に当たった。終端扱いにする。 */
        at = s_end_bound;
        goto end_of_data;
    }

    int c = vgm_getc();
    if (c < 0)
    {
        vgm_fail("truncated", at);
        return false;
    }

    uint8_t op = (uint8_t)c;

    /* YM2151 のレジスタ書き込み */
    if (op == 0x54u)
    {
        int a = vgm_getc();
        int d = vgm_getc();
        if (a < 0 || d < 0)
        {
            vgm_fail("truncated", at);
            return false;
        }
        /*
         * a は 8bit のレジスタアドレスそのもの。bit7 をマスクしてはいけない。
         * 0x80-0xFF は D1L/RR・KC・KF・PMS/AMS の実レジスタで、落とすと音が出ない。
         * 2 個目の YM2151 は別オペコード 0xA4 で、下の固定長スキップで飛ばされる。
         */
        opm_write((uint8_t)a, (uint8_t)d);
        return true;
    }

    /* wait */
    if (op == 0x61u)
    {
        int lo = vgm_getc();
        int hi = vgm_getc();
        if (lo < 0 || hi < 0)
        {
            vgm_fail("truncated", at);
            return false;
        }
        advance_samples((uint32_t)lo | ((uint32_t)hi << 8));
        return true;
    }
    if (op == 0x62u)
    {
        advance_samples(735u); /* 60Hz 1 フレーム */
        return true;
    }
    if (op == 0x63u)
    {
        advance_samples(882u); /* 50Hz 1 フレーム */
        return true;
    }
    if (op >= 0x70u && op <= 0x7Fu)
    {
        advance_samples((uint32_t)(op & 0x0Fu) + 1u);
        return true;
    }
    if (op >= 0x80u && op <= 0x8Fu)
    {
        /* YM2612 の DAC 書き込み。書き込みは飛ばすが wait は効かせる。 */
        advance_samples((uint32_t)(op & 0x0Fu));
        return true;
    }

    /* データブロック 0x67 0x66 tt ssssssss */
    if (op == 0x67u)
    {
        int compat = vgm_getc();
        int type = vgm_getc();
        uint8_t sz[4];
        for (int i = 0; i < 4; i++)
        {
            int v = vgm_getc();
            if (v < 0)
            {
                vgm_fail("truncated", at);
                return false;
            }
            sz[i] = (uint8_t)v;
        }
        if (compat < 0 || type < 0)
        {
            vgm_fail("truncated", at);
            return false;
        }
        /* 最上位ビットは長さではないので落とす */
        uint32_t size = rd32(sz) & 0x7FFFFFFFu;
        if (!vgm_skip(size))
        {
            vgm_fail("data block out of range", at);
            return false;
        }
        return true;
    }

    /* 終端 */
    if (op == 0x66u)
    {
        at = vgm_tell();
        goto end_of_data;
    }

    /* 残りは固定長スキップ */
    uint8_t n = operand_len(op);
    if (n == 0xFFu)
    {
        printf("# ERR vgm bad file (opcode 0x%02x at 0x%08x)\n", (unsigned)op, (unsigned)at);
        s_state = VGM_STATE_ERROR;
        return false;
    }
    if (n > 0u && !vgm_skip(n))
    {
        vgm_fail("truncated", at);
        return false;
    }
    return true;

end_of_data:
    if (s_loop_target != 0u)
    {
        if (!vgm_seek(s_loop_target))
        {
            vgm_fail("loop target out of range", at);
            return false;
        }
        s_loops++;
        /*
         * s_samples と s_start_us はループを跨いでも触らない。
         * 継ぎ目に時間の不連続が出ず、ずれも溜まらない。
         */
        return true;
    }

    /*
     * **ここでは OPM に何も書かない。** 最後の音は音色本来の RR で自然に減衰し、
     * その余韻を songend が RINGOUT で観測して曲の終わりを決める。ここで消音すると
     * 余韻を切り落とすうえ、キャプチャの終端も曲送りも早まる。
     *
     * 消音が走るのは vgm_stop() と、次の vgm_play() 先頭の opm_clear() だけ。
     * MDX 側 (mdx.c) も同じ扱いで、あちらはそれが参照実装の挙動そのものになる。
     */
    close_file();
    s_state = VGM_STATE_STOPPED;
    printf("# vgm     : end of data\n");
    return false;
}

/* ---- 初期化とサービス -------------------------------------------------- */

void vgm_init(void)
{
    s_state = VGM_STATE_STOPPED;
    s_open = false;
    s_gz = false;
    s_skip_left = 0;
    s_gz_reloads = 0;
    s_name[0] = '\0';
}

bool vgm_service(void)
{
    if (s_state != VGM_STATE_PLAYING)
    {
        return false;
    }

    uint64_t now = time_us_64();
    if (now < s_due_us)
    {
        return false;
    }

    uint32_t lag = (uint32_t)(now - s_due_us);
    stats_seq_lag_update(lag);

    if (lag > VGM_RESYNC_LAG_US)
    {
        /*
         * フラッシュ書き込みなどで長く止まったあと。溜まった遅れを早送りで
         * 取り返すと不自然なので、時計の方を現在時刻へ張り直す。
         */
        s_start_us = now - (s_samples * 1000000ull) / VGM_SAMPLE_RATE;
        s_due_us = now;
        s_reslips++;
    }

    uint32_t t0 = time_us_32();

    while (s_state == VGM_STATE_PLAYING)
    {
        if (time_us_64() < s_due_us)
        {
            break; /* 次のイベント時刻まで待つ */
        }
        if (!vgm_step())
        {
            break;
        }
        if (time_us_32() - t0 >= VGM_BUDGET_US)
        {
            break; /* 予算切れ。遅れは次の周回で詰める。 */
        }
    }

    if (s_state == VGM_STATE_ERROR)
    {
        key_off_all();
        close_file();
    }

    return true;
}

/* ---- ヘッダの解析 ------------------------------------------------------ */

/* 成功なら NULL、失敗ならエラー理由 */
static const char *parse_header(void)
{
    uint8_t hdr[VGM_HDR_SIZE];

    if (vgm_size() < VGM_HDR_SIZE)
    {
        return "bad file";
    }
    if (!vgm_read(hdr, sizeof(hdr)))
    {
        return "bad file";
    }

    if (memcmp(hdr + VGM_OFF_MAGIC, "Vgm ", 4) != 0)
    {
        return "bad file";
    }

    s_version = rd32(hdr + VGM_OFF_VERSION);
    s_total_samples = rd32(hdr + VGM_OFF_TOTAL);
    s_gd3_offset = rd32(hdr + VGM_OFF_GD3);

    /* YM2151 のクロックは v1.10 以降にしか無い */
    uint32_t clock = (s_version >= 0x110u) ? rd32(hdr + VGM_OFF_YM2151_CLOCK) : 0u;
    bool dual_chip = (clock & 0x40000000u) != 0u;
    s_file_clock_hz = clock & 0x3FFFFFFFu;

    uint32_t size = vgm_size();

    /* データ開始位置。v1.50 より前と、0 のときは 0x40 固定。 */
    uint32_t data_rel = rd32(hdr + VGM_OFF_DATA);
    if (s_version < 0x150u || data_rel == 0u)
    {
        s_data_start = VGM_HDR_SIZE;
    }
    else
    {
        s_data_start = VGM_OFF_DATA + data_rel;
    }
    if (s_data_start < VGM_HDR_SIZE || s_data_start >= size)
    {
        return "bad file";
    }

    /* 終端の上限。EOF オフセットと GD3 の手前のうち小さい方を採る。 */
    s_end_bound = size;
    uint32_t eof_rel = rd32(hdr + VGM_OFF_EOF);
    if (eof_rel != 0u)
    {
        uint32_t eof_abs = VGM_OFF_EOF + eof_rel;
        if (eof_abs > s_data_start && eof_abs < s_end_bound)
        {
            s_end_bound = eof_abs;
        }
    }
    uint32_t gd3_rel = s_gd3_offset;
    if (gd3_rel != 0u)
    {
        uint32_t gd3_abs = VGM_OFF_GD3 + gd3_rel;
        if (gd3_abs > s_data_start && gd3_abs < s_end_bound)
        {
            s_end_bound = gd3_abs;
        }
    }

    /* ループ先頭 */
    s_loop_target = 0u;
    uint32_t loop_rel = rd32(hdr + VGM_OFF_LOOP);
    if (loop_rel != 0u)
    {
        uint32_t loop_abs = VGM_OFF_LOOP + loop_rel;
        if (loop_abs >= s_data_start && loop_abs < s_end_bound)
        {
            s_loop_target = loop_abs;
        }
        /* 範囲外ならループ無しとして扱う（再生自体は続けられる） */
    }

    printf("# vgm     : version %u.%02x  samples %u  loop %s%s\n",
           (unsigned)((s_version >> 8) & 0xffu), (unsigned)(s_version & 0xffu),
           (unsigned)s_total_samples, s_loop_target ? "yes" : "no",
           s_gz ? "  gzip" : "");

    if (dual_chip)
    {
        printf("# warn    : dual chip file; the second chip (0xA4) is ignored\n");
    }

    return NULL;
}

/*
 * ヘッダが申告するクロックへ φM を合わせ、それでも残るずれを警告する。
 *
 * VGM の wait は 44.1kHz の絶対サンプル数なので、クロックが違ってもテンポは
 * 正確なまま。ずれるのは音程と包絡線の速さだけ。だから wait を伸縮させるのは
 * 逆効果（テンポまで狂う）。合わせるのは φM の方。
 */
static const char *apply_file_clock(void)
{
    const char *err = clockmode_follow_file(s_file_clock_hz);
    if (err != NULL)
    {
        return err;
    }

    /* 寄せたあとも残るずれだけを出す（3546895Hz の PAL 物など）。 */
    uint32_t actual = opm_clock_hz_actual();
    if (s_file_clock_hz != 0u && s_file_clock_hz != actual)
    {
        printf("# clock   : file %u Hz / phiM %u Hz (%s)\n",
               (unsigned)s_file_clock_hz, (unsigned)actual,
               (s_file_clock_hz < actual) ? "pitch goes up" : "pitch goes down");
    }

    return NULL;
}

/* ---- 操作 -------------------------------------------------------------- */

const char *vgm_play(const char *name)
{
    if (!storage_fatfs_may_access())
    {
        printf("# hint    : the filesystem is handed to the PC; run storage player first\n");
        return "wrong state";
    }
    if (storage_fs_state() != STORAGE_FS_MOUNTED)
    {
        return "no filesystem";
    }

    /* 名前の検査。VGM_DIR からの相対パスとして妥当なものだけ通す（`..` を含めない）。 */
    if (!filelist_path_ok(name))
    {
        return "bad argument";
    }

    /*
     * 何かが鳴っていても受け付ける。走っている方を先に止めてからファイルを開く。
     * VGM のストリームも MDX のファイルバッファも 1 面しか無いので、新しい曲と
     * 前の曲を並べて持てない。だから読み込みに失敗しても前の曲へは戻らず、
     * 停止状態で終わる。
     *
     * ERROR も止める対象に入れて状態を STOPPED へ正規化する（stop は冪等）。
     * mdx_state() で囲むのは、MDX_ENABLED=0 のスタブの mdx_stop() が hint を
     * 出して "unsupported" を返すため。スタブの mdx_state() は常に
     * MDX_STATE_STOPPED なので、無効ビルドではここへ入らない。
     */
    if (s_state != VGM_STATE_STOPPED)
    {
        printf("# vgm     : stopped %s\n", s_name);
        vgm_stop();
    }
    if (mdx_state() != MDX_STATE_STOPPED)
    {
        printf("# mdx     : stopped %s\n", mdx_current_name());
        mdx_stop();
    }
    s_name[0] = '\0'; /* 失敗したとき前の曲の名前を状態表示に残さない */

    char path[sizeof(VGM_DIR) + 1u + sizeof(s_name)];
    snprintf(path, sizeof(path), "%s/%s", VGM_DIR, name);

    close_file();

    FRESULT fr = f_open(&s_fp, path, FA_READ);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH)
    {
        return "not found";
    }
    if (fr != FR_OK)
    {
        return "io error";
    }
    s_open = true;

    /*
     * 圧縮の有無は拡張子ではなく先頭のマジックで決める。中身が gzip の .vgm も
     * 世の中にあるし、その逆もある。
     */
    uint8_t magic[2];
    UINT nm = 0;
    if (f_read(&s_fp, magic, sizeof(magic), &nm) != FR_OK || nm != sizeof(magic))
    {
        close_file();
        return "bad file";
    }
    if (magic[0] == 0x1Fu && magic[1] == 0x8Bu)
    {
#if VGM_VGZ_ENABLED
        if (!vgz_open(&s_fp))
        {
            close_file();
            return "bad file";
        }
        s_gz = true;
#else
        printf("# hint    : .vgz (gzip) is disabled by VGM_VGZ_ENABLED=0; gunzip before transferring\n");
        close_file();
        return "bad file";
#endif
    }
    else if (f_lseek(&s_fp, 0) != FR_OK)
    {
        close_file();
        return "io error";
    }

    const char *err = parse_header();
    if (err != NULL)
    {
        close_file();
        return err;
    }

    if (!vgm_seek(s_data_start))
    {
        close_file();
        return "bad file";
    }

    /* φM をファイルのクロックへ合わせる。s_start_us を打つ前に済ませる。 */
    err = apply_file_clock();
    if (err != NULL)
    {
        close_file();
        return err;
    }

    /*
     * レジスタを既知の状態にしてから始める。opm_reset() は 20ms ブロックし
     * I2S アンダーランが確定するので使わない（必要なら利用者が先に r を打てる）。
     */
    opm_clear();

    snprintf(s_name, sizeof(s_name), "%s", name);
    s_samples = 0;
    s_loops = 0;
    s_reslips = 0;
    s_gz_reloads = 0;
    s_play_seq++;
    s_start_us = time_us_64();
    s_due_us = s_start_us;
    s_state = VGM_STATE_PLAYING;

    return NULL;
}

/*
 * 「確実に止める」コマンドなので、止まっている状態から呼んでも成功にする
 * （storage host / clock / p 0 と同じ冪等の扱い）。
 */
const char *vgm_stop(void)
{
    /*
     * **音を止めるのは状態に関わらず行う。** 自然終了 (end of data) では OPM を
     * 触らず、その時点で状態は STOPPED になっている。下の早期 return より前に
     * 撃たないと、鳴り終わったあとの余韻を消す手段が無くなる。
     */
    key_off_all();

    if (s_state != VGM_STATE_PLAYING && s_state != VGM_STATE_ERROR)
    {
        return NULL;
    }

    close_file();
    s_state = VGM_STATE_STOPPED;
    return NULL;
}

/* ---- 一覧 -------------------------------------------------------------- */

/* 一覧に出す拡張子。gzip 圧縮された .vgz も同じ一覧に並べる。 */
static const char *const VGM_EXTS[] = {".vgm", ".vgz"};

const char *vgm_list(void (*tick)(void))
{
    return filelist_print(VGM_DIR, VGM_EXTS,
                          (uint32_t)(sizeof(VGM_EXTS) / sizeof(VGM_EXTS[0])),
                          VGM_LIST_MAX, tick);
}

const char *vgm_collect(filelist_buf_t *buf)
{
    /* 上限は s_name に収まる長さ。これより長い相対パスは vgm_play() が弾く。 */
    return filelist_collect(VGM_DIR, VGM_EXTS,
                            (uint32_t)(sizeof(VGM_EXTS) / sizeof(VGM_EXTS[0])),
                            (uint32_t)(sizeof(s_name) - 1u), buf);
}

/* ---- 問い合わせ -------------------------------------------------------- */

vgm_state_t vgm_state(void)
{
    return s_state;
}

const char *vgm_state_name(void)
{
    switch (s_state)
    {
    case VGM_STATE_PLAYING:
        return "PLAYING";
    case VGM_STATE_ERROR:
        return "ERROR";
    case VGM_STATE_STOPPED:
    default:
        return "STOPPED";
    }
}

bool vgm_is_playing(void)
{
    return s_state == VGM_STATE_PLAYING;
}

const char *vgm_current_name(void)
{
    return (s_state == VGM_STATE_STOPPED) ? "" : s_name;
}

uint64_t vgm_position_samples(void)
{
    return s_samples;
}

uint32_t vgm_total_samples(void)
{
    return s_total_samples;
}

uint32_t vgm_loop_count(void)
{
    return s_loops;
}

/*
 * 再生を始めた回数。songend / capture が「新しいトラックが始まった」を検出するのに
 * 使う。鳴っている最中に別の曲を play しても再生状態は落ちないので、レベルを
 * 見ているだけでは切り替えを拾えない。
 */
uint32_t vgm_play_seq(void)
{
    return s_play_seq;
}

uint32_t vgm_reslip_count(void)
{
    return s_reslips;
}

bool vgm_is_compressed(void)
{
    return s_gz;
}

uint32_t vgm_gz_reload_count(void)
{
    return s_gz_reloads;
}

uint32_t vgm_file_clock_hz(void)
{
    return s_file_clock_hz;
}
