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
    static FILINFO fi;
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
            if (f_readdir(&dp, &fi) != FR_OK || fi.fname[0] == '\0') {
                break; /* 終端かエラー */
            }
            if (!is_listable(&fi, exts, n_exts)) {
                continue;
            }
            if (!first && filelist_name_cmp(fi.fname, prev) <= 0) {
                continue; /* もう出した */
            }
            if (!found || filelist_name_cmp(fi.fname, best) < 0) {
                snprintf(best, sizeof(best), "%s", fi.fname);
                best_size = (uint32_t)fi.fsize;
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
