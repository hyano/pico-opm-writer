/*
 * FatFs 上のファイル一覧の出力の実装
 *
 * SPDX-License-Identifier: MIT
 */
#include "filelist.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "ff.h"

#include "storage.h"

int filelist_name_cmp(const char *a, const char *b) {
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

/*
 * 大小無視の等値判定。
 *
 * filelist_name_cmp() は使えない。あちらは大小無視で等しいときに strcmp() の
 * 結果を返して順序を一意化するので、".MDX" と ".mdx" のように大小だけが違う
 * 組では 0 にならない。並べ替えの比較子と等値判定は別物として持つ。
 */
static bool ext_equal(const char *a, const char *b) {
    for (;;) {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb) {
            return false;
        }
        if (ca == '\0') {
            return true;
        }
        a++;
        b++;
    }
}

/* 末尾が exts のいずれかと一致するか。拡張子の前に 1 文字以上を要求する。 */
static bool has_ext(const char *name, const char *const *exts, uint32_t n_exts) {
    size_t len = strlen(name);

    for (uint32_t i = 0; i < n_exts; i++) {
        size_t ext_len = strlen(exts[i]);
        if (len < ext_len + 1u) {
            continue; /* "x.vgm" が最短。拡張子そのものは名前として認めない */
        }
        if (ext_equal(name + len - ext_len, exts[i])) {
            return true;
        }
    }
    return false;
}

/*
 * 走査に使う FILINFO。FF_LFN_BUF が 255 なので 300 バイト近くあり、スタックには
 * 置かない。filelist_print() と filelist_collect() で共用するので、この 2 本は
 * 互いに再入できない（ヘッダに明記）。
 */
static FILINFO s_fi;

static bool is_listable(const FILINFO *fi, const char *const *exts, uint32_t n_exts) {
    if ((fi->fattrib & (AM_DIR | AM_HID | AM_SYS)) != 0) {
        return false;
    }
    /* macOS が作る AppleDouble */
    if (fi->fname[0] == '.') {
        return false;
    }
    return has_ext(fi->fname, exts, n_exts);
}

const char *filelist_print(const char *dir, const char *const *exts, uint32_t n_exts,
                           uint32_t max_entries, void (*tick)(void)) {
    if (!storage_fatfs_may_access()) {
        printf("# hint    : the filesystem is handed to the PC; run storage player first\n");
        return "wrong state";
    }
    if (storage_fs_state() != STORAGE_FS_MOUNTED) {
        return "no filesystem";
    }

    /*
     * 名前を全部ためてからソートすると数十 KB のバッファが要るので、
     * 「直前に出した名前より大きいものの中で最小」を毎回ディレクトリ走査で
     * 探す。走査は XIP からの読み出しだけなので速く、必要な RAM は名前 2 個ぶん。
     *
     * この 2 本は FILINFO の fname と同じ大きさ（ffconf.h の FF_LFN_BUF）。
     */
    char prev[FF_LFN_BUF + 1];
    char best[FF_LFN_BUF + 1];
    DIR dp;

    prev[0] = '\0';
    uint32_t emitted = 0;
    bool first = true;

    for (;;) {
        FRESULT fr = f_opendir(&dp, dir);
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
            if (f_readdir(&dp, &s_fi) != FR_OK || s_fi.fname[0] == '\0') {
                break; /* 終端かエラー */
            }
            if (!is_listable(&s_fi, exts, n_exts)) {
                continue;
            }
            if (!first && filelist_name_cmp(s_fi.fname, prev) <= 0) {
                continue; /* もう出した */
            }
            if (!found || filelist_name_cmp(s_fi.fname, best) < 0) {
                snprintf(best, sizeof(best), "%s", s_fi.fname);
                best_size = (uint32_t)s_fi.fsize;
                found = true;
            }
        }

        f_closedir(&dp);

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

        if (emitted >= max_entries) {
            printf("# warn    : truncated at %u entries\n", (unsigned)max_entries);
            break;
        }
    }

    printf("# files   : %u\n", (unsigned)emitted);
    return NULL;
}

/* ---- 一覧を RAM へ集める ----------------------------------------------- */

void filelist_buf_reset(filelist_buf_t *buf) {
    buf->pool_used = 0;
    buf->count = 0;
    buf->truncated = false;
}

/*
 * name を pool へ詰め、そのオフセットを offs[from..count) の中の正しい位置へ挿す。
 *
 * 走査しながら二分探索で挿入位置を決め、後ろを memmove で 1 個ずらす。全部ためてから
 * 並べ替えるのに比べて追加の RAM が要らず、動かすのは uint16 の配列だけで済む。
 * from を渡すのは、既に入っている前のディレクトリの分と混ぜないため。
 */
static bool insert_sorted(filelist_buf_t *buf, uint32_t from, const char *name) {
    size_t len = strlen(name) + 1u;

    if (buf->count >= buf->max_entries) {
        return false;
    }
    if (buf->pool_used + len > buf->pool_bytes) {
        return false;
    }

    char *dst = buf->pool + buf->pool_used;
    memcpy(dst, name, len);

    uint32_t lo = from;
    uint32_t hi = buf->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (filelist_name_cmp(buf->pool + buf->offs[mid], name) < 0) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }

    if (lo < buf->count) {
        memmove(&buf->offs[lo + 1u], &buf->offs[lo],
                (buf->count - lo) * sizeof(buf->offs[0]));
    }
    buf->offs[lo] = (uint16_t)buf->pool_used;
    buf->pool_used += (uint32_t)len;
    buf->count++;
    return true;
}

const char *filelist_collect(const char *dir, const char *const *exts, uint32_t n_exts,
                             uint32_t name_max, filelist_buf_t *buf) {
    if (!storage_fatfs_may_access()) {
        printf("# hint    : the filesystem is handed to the PC; run storage player first\n");
        return "wrong state";
    }
    if (storage_fs_state() != STORAGE_FS_MOUNTED) {
        return "no filesystem";
    }

    DIR dp;
    FRESULT fr = f_opendir(&dp, dir);
    if (fr == FR_NO_PATH || fr == FR_NO_FILE) {
        return "not found";
    }
    if (fr != FR_OK) {
        return "io error";
    }

    uint32_t from = buf->count;
    uint32_t skipped = 0;

    for (;;) {
        if (f_readdir(&dp, &s_fi) != FR_OK || s_fi.fname[0] == '\0') {
            break; /* 終端かエラー */
        }
        if (!is_listable(&s_fi, exts, n_exts)) {
            continue;
        }
        if (strlen(s_fi.fname) > name_max) {
            skipped++;
            continue;
        }
        if (!insert_sorted(buf, from, s_fi.fname)) {
            buf->truncated = true;
            break;
        }
    }

    f_closedir(&dp);

    if (skipped != 0u) {
        printf("# warn    : %s: skipped %u name(s) longer than %u chars\n", dir,
               (unsigned)skipped, (unsigned)name_max);
    }
    if (buf->truncated) {
        printf("# warn    : truncated at %u entries\n", (unsigned)buf->count);
    }

    return NULL;
}
