/*
 * gzip ストリームの展開の実装
 *
 * SPDX-License-Identifier: MIT
 */
#include "vgz.h"

#if VGM_VGZ_ENABLED

#include <stdio.h>
#include <string.h>

#include "miniz.h"

/* ---- 定数 -------------------------------------------------------------- */

/*
 * tinfl の出力リング。DEFLATE の距離は最大 32768 なので、このリングが
 * そのまま履歴窓になる。tinfl は
 *
 *     mask = (pOut_buf_next - pOut_buf_start) + *pOut_buf_size - 1
 *
 * を窓のマスクとして使い、これが 2 の冪 -1 でないと弾く。つまり
 * 「毎回リングの末尾まで埋めさせる」呼び方しかできない。
 */
#define VGZ_DICT_SIZE 32768u
#define VGZ_DICT_MASK (VGZ_DICT_SIZE - 1u)

/*
 * 1 回の tinfl_decompress() に渡す圧縮バイト数の上限。
 *
 * 出力側は上のとおり長さを指定できないので、1 回の呼び出しの仕事量は
 * 入力量で抑える。tinfl は渡した入力を使い切ると
 * TINFL_STATUS_NEEDS_MORE_INPUT で戻る。256 バイトなら通常の VGM の
 * 圧縮率（3〜10 倍）で 1〜3KB の展開に相当し、VGM_BUDGET_US に収まる。
 *
 * 圧縮率が極端に高い箇所では 1 回でリングの末尾まで（最大 32KB）展開しうる。
 * そのときは数 ms かかるが、I2S のリングが 65.5ms あるので破綻はしない。
 */
#define VGZ_IN_CHUNK 256u

/* 圧縮側の読み込み単位 */
#define VGZ_IN_SIZE 4096u

/* gzip のヘッダ最小長 + トレーラ長 */
#define VGZ_HDR_MIN 10u
#define VGZ_TRAILER 8u

/* FLG のビット */
#define VGZ_FHCRC    0x02u
#define VGZ_FEXTRA   0x04u
#define VGZ_FNAME    0x08u
#define VGZ_FCOMMENT 0x10u

/* 展開が進まない呼び出しがこれだけ続いたら壊れているとみなす */
#define VGZ_STALL_LIMIT 64u

/* ---- 状態 -------------------------------------------------------------- */

static FIL *s_fp;
static bool s_open;
static bool s_error;
static bool s_eos; /* DEFLATE ストリームが終端まで到達した */

/* 圧縮側 */
static uint8_t s_in[VGZ_IN_SIZE];
static uint32_t s_in_len;
static uint32_t s_in_pos;
static uint32_t s_in_base; /* s_in[0] のファイル内位置 */
static uint32_t s_in_next; /* 次に f_read する位置 */
static uint32_t s_deflate_start;
static uint32_t s_deflate_end; /* トレーラの手前 */
static uint32_t s_isize;

/* 展開側 */
static tinfl_decompressor s_inf;
static uint8_t s_out[VGZ_DICT_SIZE] __attribute__((aligned(4)));
static uint32_t s_dict_ofs; /* リングの次の書き込み位置 */
static uint32_t s_pend_ofs; /* 展開済みだがまだ渡していない領域 */
static uint32_t s_pend_len;
static uint32_t s_pos; /* 展開後の位置（渡した合計） */

/* スナップショット */
static bool s_snap_valid;
static tinfl_decompressor s_snap_inf;
static uint8_t s_snap_out[VGZ_DICT_SIZE] __attribute__((aligned(4)));
static uint32_t s_snap_dict_ofs;
static uint32_t s_snap_pend_ofs;
static uint32_t s_snap_pend_len;
static uint32_t s_snap_pos;
static uint32_t s_snap_in_abs; /* 消費済み圧縮バイト数（ファイル内位置） */
static bool s_snap_eos;

/* ---- 圧縮側の読み出し -------------------------------------------------- */

/* 消費済みの圧縮バイト位置 */
static uint32_t in_abs(void)
{
    return s_in_base + s_in_pos;
}

static bool refill_in(void)
{
    if (s_in_next >= s_deflate_end)
    {
        return false;
    }
    uint32_t want = s_deflate_end - s_in_next;
    if (want > VGZ_IN_SIZE)
    {
        want = VGZ_IN_SIZE;
    }
    /* 逐次読みのあいだは fptr が既に合っているので、ずれたときだけシークする。 */
    if ((uint32_t)f_tell(s_fp) != s_in_next && f_lseek(s_fp, s_in_next) != FR_OK)
    {
        return false;
    }
    UINT n = 0;
    if (f_read(s_fp, s_in, want, &n) != FR_OK || n == 0u)
    {
        return false;
    }
    s_in_base = s_in_next;
    s_in_len = (uint32_t)n;
    s_in_pos = 0;
    s_in_next += (uint32_t)n;
    return true;
}

/* 圧縮側を 1 バイト読む。gzip ヘッダの解析にだけ使う。 */
static int in_getc(void)
{
    if (s_in_pos >= s_in_len && !refill_in())
    {
        return -1;
    }
    return s_in[s_in_pos++];
}

/* 圧縮側の読み出し位置を pos へ張り直す */
static void in_reset(uint32_t pos)
{
    s_in_len = 0;
    s_in_pos = 0;
    s_in_base = pos;
    s_in_next = pos;
}

/* ---- gzip のヘッダとトレーラ ------------------------------------------- */

/* 成功なら NULL、失敗なら理由 */
static const char *read_trailer(void)
{
    uint32_t size = (uint32_t)f_size(s_fp);
    uint8_t t[VGZ_TRAILER];
    UINT n = 0;

    if (f_lseek(s_fp, size - VGZ_TRAILER) != FR_OK)
    {
        return "io error";
    }
    if (f_read(s_fp, t, sizeof(t), &n) != FR_OK || n != sizeof(t))
    {
        return "io error";
    }
    /*
     * t[0..3] は CRC32。検証はしない。ループ再生では終端に到達しないことが
     * 普通で、全部展開しないと計算できない CRC を待っても壊れたファイルの
     * 検出は早まらない。壊れていれば tinfl の展開エラーか、VGM 側の未知
     * オペコードとして必ず捕まる。
     */
    s_isize = (uint32_t)t[4] | ((uint32_t)t[5] << 8) | ((uint32_t)t[6] << 16) |
              ((uint32_t)t[7] << 24);
    if (s_isize == 0u)
    {
        s_isize = VGZ_SIZE_UNKNOWN;
    }
    return NULL;
}

/* 成功なら NULL、失敗なら理由 */
static const char *parse_gzip_header(void)
{
    in_reset(0);

    int id1 = in_getc();
    int id2 = in_getc();
    int cm = in_getc();
    int flg = in_getc();
    if (id1 != 0x1F || id2 != 0x8B)
    {
        return "bad file";
    }
    if (cm != 8)
    {
        return "bad file"; /* DEFLATE 以外の圧縮法 */
    }
    if (flg < 0 || (flg & 0xE0) != 0)
    {
        return "bad file"; /* 予約ビットが立っている */
    }
    for (int i = 0; i < 6; i++)
    { /* MTIME(4) XFL OS */
        if (in_getc() < 0)
        {
            return "bad file";
        }
    }

    if (flg & VGZ_FEXTRA)
    {
        int lo = in_getc();
        int hi = in_getc();
        if (lo < 0 || hi < 0)
        {
            return "bad file";
        }
        uint32_t xlen = (uint32_t)lo | ((uint32_t)hi << 8);
        for (uint32_t i = 0; i < xlen; i++)
        {
            if (in_getc() < 0)
            {
                return "bad file";
            }
        }
    }
    if (flg & VGZ_FNAME)
    {
        int c;
        while ((c = in_getc()) > 0)
        {
        }
        if (c < 0)
        {
            return "bad file";
        }
    }
    if (flg & VGZ_FCOMMENT)
    {
        int c;
        while ((c = in_getc()) > 0)
        {
        }
        if (c < 0)
        {
            return "bad file";
        }
    }
    if (flg & VGZ_FHCRC)
    {
        if (in_getc() < 0 || in_getc() < 0)
        {
            return "bad file";
        }
    }

    s_deflate_start = in_abs();
    if (s_deflate_start >= s_deflate_end)
    {
        return "bad file";
    }
    return NULL;
}

/* ---- 展開 -------------------------------------------------------------- */

/* 展開器とリングを初期状態へ戻す */
static void reset_inflator(void)
{
    tinfl_init(&s_inf);
    memset(s_out, 0, sizeof(s_out));
    s_dict_ofs = 0;
    s_pend_ofs = 0;
    s_pend_len = 0;
    s_pos = 0;
    s_eos = false;
    s_error = false;
    in_reset(s_deflate_start);
}

/*
 * 未消費の展開データが無いときに呼んで、リングへ次のかたまりを作る。
 * 何か作れたら true。終端かエラーなら false。
 */
static bool produce(void)
{
    if (s_error || s_eos)
    {
        return false;
    }

    for (uint32_t stall = 0; stall < VGZ_STALL_LIMIT; stall++)
    {
        if (s_in_pos >= s_in_len && !refill_in())
        {
            /* トレーラの手前で入力が尽きた = 切り詰められている */
            s_error = true;
            return false;
        }

        size_t in_n = s_in_len - s_in_pos;
        if (in_n > VGZ_IN_CHUNK)
        {
            in_n = VGZ_IN_CHUNK;
        }
        /* 出力はリングの末尾まで。長さを選べないのは冒頭のコメントのとおり。 */
        size_t out_n = VGZ_DICT_SIZE - s_dict_ofs;

        /*
         * 渡した先にもまだ圧縮データがあるかどうかを正しく伝える。最後の
         * かたまりでこれを立てたままにすると tinfl が終端を判定できない。
         */
        mz_uint32 flags = 0;
        if (s_in_pos + in_n < s_in_len || s_in_next < s_deflate_end)
        {
            flags = TINFL_FLAG_HAS_MORE_INPUT;
        }

        tinfl_status st = tinfl_decompress(&s_inf, s_in + s_in_pos, &in_n, s_out,
                                           s_out + s_dict_ofs, &out_n, flags);

        s_in_pos += (uint32_t)in_n;
        s_pend_ofs = s_dict_ofs;
        s_pend_len = (uint32_t)out_n;
        s_dict_ofs = (s_dict_ofs + (uint32_t)out_n) & VGZ_DICT_MASK;

        if (st < 0)
        {
            s_error = true;
            return false;
        }
        if (st == TINFL_STATUS_DONE)
        {
            s_eos = true;
            return s_pend_len > 0u;
        }
        if (s_pend_len > 0u)
        {
            return true;
        }
        /* まだ何も出ていない（動的ハフマンの表を読んでいる途中など）。続ける。 */
    }

    s_error = true;
    return false;
}

/* ---- 公開関数 ---------------------------------------------------------- */

bool vgz_open(FIL *fp)
{
    s_open = false;
    s_snap_valid = false;
    s_isize = VGZ_SIZE_UNKNOWN;
    s_fp = fp;

    uint32_t size = (uint32_t)f_size(fp);
    if (size < VGZ_HDR_MIN + VGZ_TRAILER + 2u)
    {
        return false;
    }
    s_deflate_end = size - VGZ_TRAILER;

    if (read_trailer() != NULL)
    {
        return false;
    }
    if (parse_gzip_header() != NULL)
    {
        return false;
    }

    reset_inflator();
    s_open = true;
    return true;
}

void vgz_close(void)
{
    s_open = false;
    s_snap_valid = false;
    s_fp = NULL;
    s_in_len = 0;
    s_in_pos = 0;
    s_pend_len = 0;
}

uint32_t vgz_read(void *dst, uint32_t n)
{
    if (!s_open)
    {
        return 0;
    }
    uint8_t *p = (uint8_t *)dst;
    uint32_t got = 0;

    while (got < n)
    {
        if (s_pend_len == 0u && !produce())
        {
            break;
        }
        uint32_t take = n - got;
        if (take > s_pend_len)
        {
            take = s_pend_len;
        }
        /*
         * tinfl はリングの末尾を越えて書かないので、未消費の領域は必ず連続。
         * 折り返しの処理は要らない。
         */
        memcpy(p + got, s_out + s_pend_ofs, take);
        s_pend_ofs = (s_pend_ofs + take) & VGZ_DICT_MASK;
        s_pend_len -= take;
        s_pos += take;
        got += take;
    }
    return got;
}

bool vgz_error(void)
{
    return s_error;
}

uint32_t vgz_tell(void)
{
    return s_pos;
}

uint32_t vgz_isize(void)
{
    return s_isize;
}

void vgz_snapshot(void)
{
    if (!s_open)
    {
        return;
    }
    memcpy(&s_snap_inf, &s_inf, sizeof(s_snap_inf));
    memcpy(s_snap_out, s_out, sizeof(s_snap_out));
    s_snap_dict_ofs = s_dict_ofs;
    s_snap_pend_ofs = s_pend_ofs;
    s_snap_pend_len = s_pend_len;
    s_snap_pos = s_pos;
    s_snap_in_abs = in_abs();
    s_snap_eos = s_eos;
    s_snap_valid = true;
}

bool vgz_restore(void)
{
    if (!s_open || !s_snap_valid)
    {
        return false;
    }
    memcpy(&s_inf, &s_snap_inf, sizeof(s_inf));
    memcpy(s_out, s_snap_out, sizeof(s_out));
    s_dict_ofs = s_snap_dict_ofs;
    s_pend_ofs = s_snap_pend_ofs;
    s_pend_len = s_snap_pend_len;
    s_pos = s_snap_pos;
    s_eos = s_snap_eos;
    s_error = false;
    /*
     * 圧縮側は保存した位置から読み直す。s_in の中身は捨てる。ビットバッファは
     * s_inf の側に入っているので、バイト境界の途中でも正しく続きが読める。
     */
    in_reset(s_snap_in_abs);
    return true;
}

bool vgz_snapshot_valid(void)
{
    return s_snap_valid;
}

bool vgz_rewind(void)
{
    if (!s_open)
    {
        return false;
    }
    reset_inflator();
    return true;
}

#else /* VGM_VGZ_ENABLED */

bool vgz_open(FIL *fp)
{
    (void)fp;
    return false;
}
void vgz_close(void) {}
uint32_t vgz_read(void *dst, uint32_t n)
{
    (void)dst;
    (void)n;
    return 0;
}
bool vgz_error(void)
{
    return true;
}
uint32_t vgz_tell(void)
{
    return 0;
}
uint32_t vgz_isize(void)
{
    return VGZ_SIZE_UNKNOWN;
}
void vgz_snapshot(void) {}
bool vgz_restore(void)
{
    return false;
}
bool vgz_snapshot_valid(void)
{
    return false;
}
bool vgz_rewind(void)
{
    return false;
}

#endif /* VGM_VGZ_ENABLED */
