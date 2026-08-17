/*
 * VGM ファイルの再生の実装
 *
 * SPDX-License-Identifier: MIT
 */
#include "vgm.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "ff.h"

#include "opm.h"
#include "stats.h"
#include "storage.h"

/* ---- ヘッダのオフセット ------------------------------------------------ */

#define VGM_HDR_SIZE 0x40u

#define VGM_OFF_MAGIC 0x00u
#define VGM_OFF_EOF 0x04u
#define VGM_OFF_VERSION 0x08u
#define VGM_OFF_GD3 0x14u
#define VGM_OFF_TOTAL 0x18u
#define VGM_OFF_LOOP 0x1Cu
#define VGM_OFF_LOOP_SAMPLES 0x20u
#define VGM_OFF_YM2151_CLOCK 0x30u
#define VGM_OFF_DATA 0x34u

/* ---- 状態 -------------------------------------------------------------- */

static vgm_state_t s_state;

static FIL s_fp;
static bool s_open;

static char s_name[64];

/* ヘッダから読んだもの */
static uint32_t s_version;
static uint32_t s_file_clock_hz;
static uint32_t s_total_samples;
static uint32_t s_gd3_offset;   /* 将来 GD3 を読むときのために保持しておく */
static uint32_t s_data_start;
static uint32_t s_loop_target;  /* 0 ならループしない */
static uint32_t s_end_bound;    /* ここに達したら終端として扱う */

/* スケジューラ */
static uint64_t s_start_us;
static uint64_t s_samples; /* 発行済みコマンドまでのサンプル数（ループを跨いで連続） */
static uint64_t s_due_us;
static uint32_t s_loops;
static uint32_t s_reslips;

/*
 * ストリーム用のバッファ。1 クラスタぶん読むので f_read は FIL の 512 バイト窓を
 * 通らず disk_read(count=8) に直行し、XIP からの memcpy 1 回で済む。
 * 複数バイトのコマンドは vgm_getc() を繰り返し呼ぶだけなので、境界処理は要らない。
 */
static uint8_t s_buf[4096];
static uint32_t s_buf_len;
static uint32_t s_buf_pos;
static uint32_t s_buf_base; /* s_buf[0] のファイル内位置 */

/* ---- バイト列の読み出し ------------------------------------------------ */

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* 現在のファイル内位置 */
static uint32_t vgm_tell(void) {
    return s_buf_base + s_buf_pos;
}

static bool vgm_refill(void) {
    UINT n = 0;
    s_buf_base = (uint32_t)f_tell(&s_fp);
    if (f_read(&s_fp, s_buf, sizeof(s_buf), &n) != FR_OK || n == 0u) {
        s_buf_len = 0;
        s_buf_pos = 0;
        return false;
    }
    s_buf_len = (uint32_t)n;
    s_buf_pos = 0;
    return true;
}

/* 1 バイト読む。終端・エラーなら -1。 */
static int vgm_getc(void) {
    if (s_buf_pos >= s_buf_len) {
        if (!vgm_refill()) {
            return -1;
        }
    }
    return s_buf[s_buf_pos++];
}

/* n バイト読み飛ばす */
static bool vgm_skip(uint32_t n) {
    uint32_t in_buf = s_buf_len - s_buf_pos;
    if (n <= in_buf) {
        s_buf_pos += n;
        return true;
    }

    /*
     * バッファに無い分はシークで飛ばす。0x67 のデータブロックは MB 単位に
     * なりうるので、1 バイトずつ読んで飛ばしてはいけない。
     */
    uint32_t target = vgm_tell() + n;
    if (target > (uint32_t)f_size(&s_fp)) {
        return false;
    }
    if (f_lseek(&s_fp, target) != FR_OK) {
        return false;
    }
    s_buf_len = 0;
    s_buf_pos = 0;
    s_buf_base = target;
    return true;
}

static bool vgm_seek(uint32_t pos) {
    if (pos > (uint32_t)f_size(&s_fp)) {
        return false;
    }
    if (f_lseek(&s_fp, pos) != FR_OK) {
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
static uint8_t operand_len(uint8_t op) {
    if (op <= 0x2Fu) {
        return 0xFFu; /* 未知 */
    }
    if (op <= 0x3Fu) {
        return 1u; /* 0x30-0x3F: 0x50-0x5F の 2 個目のチップ */
    }
    if (op <= 0x4Eu) {
        /* 0x40-0x4E は v1.60 でオペランドが 1 -> 2 に増えた */
        return (s_version >= 0x160u) ? 2u : 1u;
    }
    if (op == 0x4Fu) {
        return 1u; /* GG PSG ステレオ */
    }
    if (op == 0x50u) {
        return 1u; /* SN76489 */
    }
    if (op <= 0x5Fu) {
        return 2u; /* 0x51-0x5F: YM2413 / YM2612 / YM2203 ... */
    }
    if (op == 0x64u) {
        return 3u; /* 0x62/0x63 の長さ上書き（非推奨） */
    }
    if (op == 0x68u) {
        return 11u; /* PCM RAM write */
    }
    if (op >= 0x90u && op <= 0x95u) {
        static const uint8_t dac_stream[6] = {4u, 4u, 5u, 10u, 1u, 4u};
        return dac_stream[op - 0x90u]; /* 0x90-0x95: DAC ストリーム制御 */
    }
    if (op >= 0xA0u && op <= 0xBFu) {
        return 2u;
    }
    if (op >= 0xC0u && op <= 0xDFu) {
        return 3u;
    }
    if (op >= 0xE0u) {
        return 4u;
    }
    return 0xFFu; /* 0x60 / 0x65 / 0x69-0x6F / 0x96-0x9F */
}

/* ---- エラー ------------------------------------------------------------ */

/*
 * 再生中に見つかった異常は、コマンドとしては既に OK を返したあとなので
 * 非同期通知にする（capture.c が DMA overrun でやっているのと同じ）。
 */
static void vgm_fail(const char *why, uint32_t at) {
    printf("# ERR vgm bad file (%s at 0x%08x)\n", why, (unsigned)at);
    s_state = VGM_STATE_ERROR;
}

/* ---- 停止 -------------------------------------------------------------- */

static void key_off_all(void) {
    /*
     * 全 8 チャンネルをキーオフする。8 回 x 32us = 約 256us。
     * opm_reset() は 20ms ブロックして I2S アンダーランが確定するので使わない。
     */
    for (uint8_t ch = 0; ch < 8u; ch++) {
        opm_write(0x08u, ch);
    }
}

static void close_file(void) {
    if (s_open) {
        f_close(&s_fp);
        s_open = false;
    }
    s_buf_len = 0;
    s_buf_pos = 0;
}

/* ---- 1 コマンドの実行 -------------------------------------------------- */

/* サンプル数を進めて次の予定時刻を計算する */
static void advance_samples(uint32_t n) {
    s_samples += n;
    /*
     * 絶対サンプル数から毎回計算し直すので、丸め誤差が累積しない。
     * s_samples * 1000000 が u64 を溢れるのは 13 年ぶん再生したあと。
     */
    s_due_us = s_start_us + (s_samples * 1000000ull) / VGM_SAMPLE_RATE;
}

/* 続行できるなら true。停止・エラーなら false。 */
static bool vgm_step(void) {
    uint32_t at = vgm_tell();

    if (at >= s_end_bound) {
        /* GD3 や EOF の境界に当たった。終端扱いにする。 */
        at = s_end_bound;
        goto end_of_data;
    }

    int c = vgm_getc();
    if (c < 0) {
        vgm_fail("truncated", at);
        return false;
    }

    uint8_t op = (uint8_t)c;

    /* YM2151 のレジスタ書き込み */
    if (op == 0x54u) {
        int a = vgm_getc();
        int d = vgm_getc();
        if (a < 0 || d < 0) {
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
    if (op == 0x61u) {
        int lo = vgm_getc();
        int hi = vgm_getc();
        if (lo < 0 || hi < 0) {
            vgm_fail("truncated", at);
            return false;
        }
        advance_samples((uint32_t)lo | ((uint32_t)hi << 8));
        return true;
    }
    if (op == 0x62u) {
        advance_samples(735u); /* 60Hz 1 フレーム */
        return true;
    }
    if (op == 0x63u) {
        advance_samples(882u); /* 50Hz 1 フレーム */
        return true;
    }
    if (op >= 0x70u && op <= 0x7Fu) {
        advance_samples((uint32_t)(op & 0x0Fu) + 1u);
        return true;
    }
    if (op >= 0x80u && op <= 0x8Fu) {
        /* YM2612 の DAC 書き込み。書き込みは飛ばすが wait は効かせる。 */
        advance_samples((uint32_t)(op & 0x0Fu));
        return true;
    }

    /* データブロック 0x67 0x66 tt ssssssss */
    if (op == 0x67u) {
        int compat = vgm_getc();
        int type = vgm_getc();
        uint8_t sz[4];
        for (int i = 0; i < 4; i++) {
            int v = vgm_getc();
            if (v < 0) {
                vgm_fail("truncated", at);
                return false;
            }
            sz[i] = (uint8_t)v;
        }
        if (compat < 0 || type < 0) {
            vgm_fail("truncated", at);
            return false;
        }
        /* 最上位ビットは長さではないので落とす */
        uint32_t size = rd32(sz) & 0x7FFFFFFFu;
        if (!vgm_skip(size)) {
            vgm_fail("data block out of range", at);
            return false;
        }
        return true;
    }

    /* 終端 */
    if (op == 0x66u) {
        at = vgm_tell();
        goto end_of_data;
    }

    /* 残りは固定長スキップ */
    uint8_t n = operand_len(op);
    if (n == 0xFFu) {
        printf("# ERR vgm bad file (opcode 0x%02x at 0x%08x)\n", (unsigned)op, (unsigned)at);
        s_state = VGM_STATE_ERROR;
        return false;
    }
    if (n > 0u && !vgm_skip(n)) {
        vgm_fail("truncated", at);
        return false;
    }
    return true;

end_of_data:
    if (s_loop_target != 0u) {
        if (!vgm_seek(s_loop_target)) {
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

    key_off_all();
    close_file();
    s_state = VGM_STATE_STOPPED;
    printf("# vgm     : end of data\n");
    return false;
}

/* ---- 初期化とサービス -------------------------------------------------- */

void vgm_init(void) {
    s_state = VGM_STATE_STOPPED;
    s_open = false;
    s_name[0] = '\0';
}

bool vgm_service(void) {
    if (s_state != VGM_STATE_PLAYING) {
        return false;
    }

    uint64_t now = time_us_64();
    if (now < s_due_us) {
        return false;
    }

    uint32_t lag = (uint32_t)(now - s_due_us);
    stats_vgm_lag_update(lag);

    if (lag > VGM_RESYNC_LAG_US) {
        /*
         * フラッシュ書き込みなどで長く止まったあと。溜まった遅れを早送りで
         * 取り返すと不自然なので、時計の方を現在時刻へ張り直す。
         */
        s_start_us = now - (s_samples * 1000000ull) / VGM_SAMPLE_RATE;
        s_due_us = now;
        s_reslips++;
    }

    uint32_t t0 = time_us_32();

    while (s_state == VGM_STATE_PLAYING) {
        if (time_us_64() < s_due_us) {
            break; /* 次のイベント時刻まで待つ */
        }
        if (!vgm_step()) {
            break;
        }
        if (time_us_32() - t0 >= VGM_BUDGET_US) {
            break; /* 予算切れ。遅れは次の周回で詰める。 */
        }
    }

    if (s_state == VGM_STATE_ERROR) {
        key_off_all();
        close_file();
    }

    return true;
}

/* ---- ヘッダの解析 ------------------------------------------------------ */

/* 成功なら NULL、失敗ならエラー理由 */
static const char *parse_header(void) {
    uint8_t hdr[VGM_HDR_SIZE];
    UINT n = 0;

    if (f_size(&s_fp) < VGM_HDR_SIZE) {
        return "bad file";
    }
    if (f_read(&s_fp, hdr, sizeof(hdr), &n) != FR_OK || n != sizeof(hdr)) {
        return "io error";
    }

    if (hdr[0] == 0x1Fu && hdr[1] == 0x8Bu) {
        printf("# hint    : .vgz (gzip) は非対応。gunzip してから転送すること\n");
        return "bad file";
    }
    if (memcmp(hdr + VGM_OFF_MAGIC, "Vgm ", 4) != 0) {
        return "bad file";
    }

    s_version = rd32(hdr + VGM_OFF_VERSION);
    s_total_samples = rd32(hdr + VGM_OFF_TOTAL);
    s_gd3_offset = rd32(hdr + VGM_OFF_GD3);

    /* YM2151 のクロックは v1.10 以降にしか無い */
    uint32_t clock = (s_version >= 0x110u) ? rd32(hdr + VGM_OFF_YM2151_CLOCK) : 0u;
    bool dual_chip = (clock & 0x40000000u) != 0u;
    s_file_clock_hz = clock & 0x3FFFFFFFu;

    uint32_t size = (uint32_t)f_size(&s_fp);

    /* データ開始位置。v1.50 より前と、0 のときは 0x40 固定。 */
    uint32_t data_rel = rd32(hdr + VGM_OFF_DATA);
    if (s_version < 0x150u || data_rel == 0u) {
        s_data_start = VGM_HDR_SIZE;
    } else {
        s_data_start = VGM_OFF_DATA + data_rel;
    }
    if (s_data_start < VGM_HDR_SIZE || s_data_start >= size) {
        return "bad file";
    }

    /* 終端の上限。EOF オフセットと GD3 の手前のうち小さい方を採る。 */
    s_end_bound = size;
    uint32_t eof_rel = rd32(hdr + VGM_OFF_EOF);
    if (eof_rel != 0u) {
        uint32_t eof_abs = VGM_OFF_EOF + eof_rel;
        if (eof_abs > s_data_start && eof_abs < s_end_bound) {
            s_end_bound = eof_abs;
        }
    }
    uint32_t gd3_rel = s_gd3_offset;
    if (gd3_rel != 0u) {
        uint32_t gd3_abs = VGM_OFF_GD3 + gd3_rel;
        if (gd3_abs > s_data_start && gd3_abs < s_end_bound) {
            s_end_bound = gd3_abs;
        }
    }

    /* ループ先頭 */
    s_loop_target = 0u;
    uint32_t loop_rel = rd32(hdr + VGM_OFF_LOOP);
    if (loop_rel != 0u) {
        uint32_t loop_abs = VGM_OFF_LOOP + loop_rel;
        if (loop_abs >= s_data_start && loop_abs < s_end_bound) {
            s_loop_target = loop_abs;
        }
        /* 範囲外ならループ無しとして扱う（再生自体は続けられる） */
    }

    printf("# vgm     : version %u.%02x  samples %u  loop %s\n",
           (unsigned)((s_version >> 8) & 0xffu), (unsigned)(s_version & 0xffu),
           (unsigned)s_total_samples, s_loop_target ? "yes" : "no");

    if (dual_chip) {
        printf("# warn    : dual chip ファイル。2 個目 (0xA4) は無視する\n");
    }

    /*
     * VGM の wait は 44.1kHz の絶対サンプル数なので、クロックが違ってもテンポは
     * 正確なまま。ずれるのは音程と包絡線の速さだけ。だから wait を伸縮させるのは
     * 逆効果（テンポまで狂う）。
     */
    uint32_t actual = opm_clock_hz_actual();
    if (s_file_clock_hz != 0u && s_file_clock_hz != actual) {
        printf("# clock   : file %u Hz / phiM %u Hz (%s)\n",
               (unsigned)s_file_clock_hz, (unsigned)actual,
               (s_file_clock_hz < actual) ? "音程が高くなる" : "音程が低くなる");
    }

    return NULL;
}

/* ---- 操作 -------------------------------------------------------------- */

const char *vgm_play(const char *name) {
    if (!storage_fatfs_may_access()) {
        return "wrong state";
    }
    if (storage_fs_state() != STORAGE_FS_MOUNTED) {
        return "no filesystem";
    }
    if (s_state == VGM_STATE_PLAYING) {
        return "wrong state";
    }

    /* 名前の検査。ディレクトリを跨がせない。 */
    size_t len = strlen(name);
    if (len == 0u || len > sizeof(s_name) - 1u) {
        return "bad argument";
    }
    if (strchr(name, '/') != NULL || strchr(name, '\\') != NULL) {
        return "bad argument";
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return "bad argument";
    }

    char path[8 + sizeof(s_name)];
    snprintf(path, sizeof(path), "%s/%s", VGM_DIR, name);

    close_file();

    FRESULT fr = f_open(&s_fp, path, FA_READ);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
        return "not found";
    }
    if (fr != FR_OK) {
        return "io error";
    }
    s_open = true;

    const char *err = parse_header();
    if (err != NULL) {
        close_file();
        return err;
    }

    if (!vgm_seek(s_data_start)) {
        close_file();
        return "bad file";
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
    s_start_us = time_us_64();
    s_due_us = s_start_us;
    s_state = VGM_STATE_PLAYING;

    return NULL;
}

const char *vgm_stop(void) {
    if (s_state != VGM_STATE_PLAYING && s_state != VGM_STATE_ERROR) {
        return "wrong state";
    }

    key_off_all();
    close_file();
    s_state = VGM_STATE_STOPPED;
    return NULL;
}

/* ---- 一覧 -------------------------------------------------------------- */

/* 大小を無視した比較。同じなら大小を見て決める（順序を一意にするため）。 */
static int name_cmp(const char *a, const char *b) {
    const char *pa = a;
    const char *pb = b;
    for (;;) {
        int ca = tolower((unsigned char)*pa);
        int cb = tolower((unsigned char)*pb);
        if (ca != cb) {
            return (ca < cb) ? -1 : 1;
        }
        if (ca == '\0') {
            break;
        }
        pa++;
        pb++;
    }
    return strcmp(a, b);
}

/* 拡張子が .vgm（大小無視）か */
static bool has_vgm_ext(const char *name) {
    size_t len = strlen(name);
    if (len < 5u) { /* "x.vgm" が最短 */
        return false;
    }
    const char *ext = name + len - 4u;
    return ext[0] == '.' && tolower((unsigned char)ext[1]) == 'v' &&
           tolower((unsigned char)ext[2]) == 'g' && tolower((unsigned char)ext[3]) == 'm';
}

static bool is_listable(const FILINFO *fi) {
    if ((fi->fattrib & (AM_DIR | AM_HID | AM_SYS)) != 0) {
        return false;
    }
    /* macOS が作る AppleDouble */
    if (fi->fname[0] == '.') {
        return false;
    }
    return has_vgm_ext(fi->fname);
}

const char *vgm_list(void (*tick)(void)) {
    if (!storage_fatfs_may_access()) {
        return "wrong state";
    }
    if (storage_fs_state() != STORAGE_FS_MOUNTED) {
        return "no filesystem";
    }

    /*
     * 名前を全部ためてからソートすると数十 KB のバッファが要るので、
     * 「直前に出した名前より大きいものの中で最小」を毎回ディレクトリ走査で
     * 探す。走査は XIP からの読み出しだけなので速く、必要な RAM は名前 2 個ぶん。
     */
    char prev[FF_LFN_BUF + 1];
    char best[FF_LFN_BUF + 1];
    static FILINFO fi;
    DIR dir;

    prev[0] = '\0';
    uint32_t emitted = 0;
    bool first = true;

    for (;;) {
        FRESULT fr = f_opendir(&dir, VGM_DIR);
        if (fr == FR_NO_PATH || fr == FR_NO_FILE) {
            return "not found";
        }
        if (fr != FR_OK) {
            return "io error";
        }

        bool found = false;
        uint32_t best_size = 0;
        best[0] = '\0';

        for (;;) {
            if (f_readdir(&dir, &fi) != FR_OK || fi.fname[0] == '\0') {
                break; /* 終端かエラー */
            }
            if (!is_listable(&fi)) {
                continue;
            }
            if (!first && name_cmp(fi.fname, prev) <= 0) {
                continue; /* もう出した */
            }
            if (!found || name_cmp(fi.fname, best) < 0) {
                snprintf(best, sizeof(best), "%s", fi.fname);
                best_size = (uint32_t)fi.fsize;
                found = true;
            }
        }

        f_closedir(&dir);

        if (!found) {
            break;
        }

        /* サイズを先に置く。名前は空白を含みうるので必ず最後の欄にする。 */
        printf("# file    : %9u %s\n", (unsigned)best_size, best);
        if (tick != NULL) {
            tick();
        }

        snprintf(prev, sizeof(prev), "%s", best);
        first = false;
        emitted++;

        if (emitted >= VGM_LIST_MAX) {
            printf("# warn    : %u 件で打ち切った\n", (unsigned)VGM_LIST_MAX);
            break;
        }
    }

    printf("# files   : %u\n", (unsigned)emitted);
    return NULL;
}

/* ---- 問い合わせ -------------------------------------------------------- */

vgm_state_t vgm_state(void) {
    return s_state;
}

const char *vgm_state_name(void) {
    switch (s_state) {
    case VGM_STATE_PLAYING:
        return "PLAYING";
    case VGM_STATE_ERROR:
        return "ERROR";
    case VGM_STATE_STOPPED:
    default:
        return "STOPPED";
    }
}

bool vgm_is_playing(void) {
    return s_state == VGM_STATE_PLAYING;
}

const char *vgm_current_name(void) {
    return (s_state == VGM_STATE_STOPPED) ? "" : s_name;
}

uint64_t vgm_position_samples(void) {
    return s_samples;
}

uint32_t vgm_total_samples(void) {
    return s_total_samples;
}

uint32_t vgm_loop_count(void) {
    return s_loops;
}

uint32_t vgm_reslip_count(void) {
    return s_reslips;
}

uint32_t vgm_file_clock_hz(void) {
    return s_file_clock_hz;
}
