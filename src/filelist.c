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

/* ルートのパス（"/VGM" など）に許す長さ。呼び出し側は定数を渡すので余裕をみた値。 */
#define FILELIST_ROOT_MAX 32u

/* 走査中の絶対パスを入れるバッファ。"<ルート>/<相対パス>" + NUL */
#define FILELIST_PATH_BUF (FILELIST_ROOT_MAX + 1u + FILELIST_PATH_MAX + 1u)

int filelist_name_cmp(const char *a, const char *b)
{
    const char *pa = a;
    const char *pb = b;
    for (;;)
    {
        int ca = tolower((unsigned char)*pa);
        int cb = tolower((unsigned char)*pb);
        if (ca != cb)
        {
            return (ca < cb) ? -1 : 1;
        }
        if (ca == '\0')
        {
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
static bool ext_equal(const char *a, const char *b)
{
    for (;;)
    {
        int ca = tolower((unsigned char)*a);
        int cb = tolower((unsigned char)*b);
        if (ca != cb)
        {
            return false;
        }
        if (ca == '\0')
        {
            return true;
        }
        a++;
        b++;
    }
}

/* 末尾が exts のいずれかと一致するか。拡張子の前に 1 文字以上を要求する。 */
static bool has_ext(const char *name, const char *const *exts, uint32_t n_exts)
{
    size_t len = strlen(name);

    for (uint32_t i = 0; i < n_exts; i++)
    {
        size_t ext_len = strlen(exts[i]);
        if (len < ext_len + 1u)
        {
            continue; /* "x.vgm" が最短。拡張子そのものは名前として認めない */
        }
        if (ext_equal(name + len - ext_len, exts[i]))
        {
            return true;
        }
    }
    return false;
}

bool filelist_path_ok(const char *rel)
{
    size_t len = strlen(rel);

    if (len == 0u || len > FILELIST_PATH_MAX)
    {
        return false;
    }
    if (rel[0] == '/' || rel[len - 1u] == '/')
    {
        return false;
    }

    uint32_t depth = 1u; /* 要素の数。区切りが 1 個増えるごとに 1 段深くなる */
    const char *seg = rel;

    for (const char *p = rel;; p++)
    {
        unsigned char ch = (unsigned char)*p;

        if (ch != '\0' && ch != '/')
        {
            /* `\` と `:` はドライブやパスの区切りに解釈されうるので通さない */
            if (ch < 0x20u || ch == '\\' || ch == ':')
            {
                return false;
            }
            continue;
        }

        /* ここまでが 1 要素。空・`.`・`..` は弾く（`..` を通すとルートの外へ出られる） */
        size_t seg_len = (size_t)(p - seg);
        if (seg_len == 0u)
        {
            return false;
        }
        if ((seg_len == 1u && seg[0] == '.') ||
            (seg_len == 2u && seg[0] == '.' && seg[1] == '.'))
        {
            return false;
        }

        if (ch == '\0')
        {
            break;
        }
        depth++;
        if (depth > FILELIST_MAX_DEPTH)
        {
            return false;
        }
        seg = p + 1;
    }

    return true;
}

/* ---- 集めたものを詰めるバッファ ---------------------------------------- */

void filelist_buf_reset(filelist_buf_t *buf)
{
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
static bool insert_sorted(filelist_buf_t *buf, uint32_t from, const char *name)
{
    size_t len = strlen(name) + 1u;

    if (buf->count >= buf->max_entries)
    {
        return false;
    }
    if (buf->pool_used + len > buf->pool_bytes)
    {
        return false;
    }

    char *dst = buf->pool + buf->pool_used;
    memcpy(dst, name, len);

    uint32_t lo = from;
    uint32_t hi = buf->count;
    while (lo < hi)
    {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (filelist_name_cmp(buf->pool + buf->offs[mid], name) < 0)
        {
            lo = mid + 1u;
        }
        else
        {
            hi = mid;
        }
    }

    if (lo < buf->count)
    {
        memmove(&buf->offs[lo + 1u], &buf->offs[lo],
                (buf->count - lo) * sizeof(buf->offs[0]));
    }
    buf->offs[lo] = (uint16_t)buf->pool_used;
    buf->pool_used += (uint32_t)len;
    buf->count++;
    return true;
}

/* ---- 走査の共有状態 ---------------------------------------------------- */

/*
 * 走査に使う FILINFO。FF_LFN_BUF が 255 なので 300 バイト近くあり、スタックには
 * 置かない。filelist_print() と filelist_collect() で共用するので、この 2 本は
 * 互いに再入できない（ヘッダに明記）。
 *
 * 以下の作業領域も同じ理由で static にしてある。再帰で段ごとにスタックへ
 * 名前 2 本（512 バイト）を積むわけにはいかない。
 */
static FILINFO s_fi;
static DIR s_dp[FILELIST_MAX_DEPTH]; /* 段ごとのディレクトリハンドル */
static char s_path[FILELIST_PATH_BUF]; /* いま見ているディレクトリの絶対パス */
static char s_prev[FF_LFN_BUF + 1];    /* 直前に採った名前 */
static char s_best[FF_LFN_BUF + 1];    /* 走査で見つかった最小の名前 */

/*
 * s_prev / s_best は段をまたいで共用できる。
 *
 * - ファイルの走査中に再帰は起きない（同じ段のファイルを出し切ってから降りる）ので、
 *   ファイル用の s_prev は段をまたがない。
 * - サブディレクトリ用の「直前」だけは再帰を跨ぐが、選んだ名前は s_path へ追記して
 *   降りるので、**戻ってきたときの s_path の最終要素がそのまま「直前」になる**。
 *   親を開き直すときは s_path の区切りを一時的に '\0' へ差し替えて f_opendir() し、
 *   直後に '/' へ戻す（FatFs はパス文字列を読むだけ）。
 * - s_best は s_path へ写すか出力するまでの一時領域で、再帰を跨がない。
 */

/* 走査で持ち回る状態。print と collect の違いは buf が NULL かどうかだけ。 */
typedef struct
{
    const char *const *exts;
    uint32_t n_exts;
    uint32_t path_max;  /* 相対パスに許す長さ */
    uint32_t root_len;  /* s_path のうちルートが占める長さ */
    uint32_t skip_long; /* 相対パスが長すぎて飛ばした数 */
    uint32_t skip_deep; /* 深すぎて降りなかったディレクトリ数 */
    bool truncated;     /* 上限に当たって打ち切ったか */

    /* filelist_print() 用 */
    uint32_t emitted;
    uint32_t max_entries;
    void (*tick)(void);

    /* filelist_collect() 用。NULL なら出力する。 */
    filelist_buf_t *buf;
} walk_t;

static bool is_listable(const FILINFO *fi, const char *const *exts, uint32_t n_exts)
{
    if ((fi->fattrib & (AM_DIR | AM_HID | AM_SYS)) != 0)
    {
        return false;
    }
    /* macOS が作る AppleDouble */
    if (fi->fname[0] == '.')
    {
        return false;
    }
    return has_ext(fi->fname, exts, n_exts);
}

/*
 * 潜る対象のディレクトリか。
 *
 * FatFs の f_readdir() は "." と ".." を返さない（ff.c の dir_read() が弾く）が、
 * 先頭ドットの除外でどのみち通らない。
 */
static bool is_walkable_dir(const FILINFO *fi)
{
    if ((fi->fattrib & AM_DIR) == 0)
    {
        return false;
    }
    if ((fi->fattrib & (AM_HID | AM_SYS)) != 0)
    {
        return false;
    }
    return fi->fname[0] != '.';
}

/* base 文字目までが親のパスのとき、name を足した相対パスが長さの上限に収まるか。 */
static bool fits(const walk_t *w, size_t base, const char *name)
{
    size_t nlen = strlen(name);

    /* base == root_len のときは相対パスが name そのものになる */
    if (base + nlen - w->root_len > w->path_max)
    {
        return false;
    }
    return base + 1u + nlen + 1u <= sizeof(s_path);
}

/*
 * s_path の base 文字目以降を "/name" にする。fits() で確認してから呼ぶこと。
 *
 * snprintf() を使わないのは、コンパイラが fits() の確認を追えず
 * -Wformat-truncation を出すため。頭打ちは通常は働かない保険。
 */
static void path_push(size_t base, const char *name)
{
    if (base + 2u > sizeof(s_path))
    {
        return;
    }

    size_t room = sizeof(s_path) - base - 2u; /* '/' と NUL の分を引く */
    size_t nlen = strlen(name);

    if (nlen > room)
    {
        nlen = room;
    }
    s_path[base] = '/';
    memcpy(&s_path[base + 1u], name, nlen);
    s_path[base + 1u + nlen] = '\0';
}

/*
 * s_path のディレクトリを 1 周し、prev より大きい名前のうち最小のものを s_best へ。
 *
 * 名前を全部ためてからソートすると数十 KB のバッファが要るので、「直前に採った名前より
 * 大きいものの中で最小」を毎回ディレクトリ走査で探す。走査は XIP からの読み出しだけなので
 * 速く、必要な RAM は名前 2 個ぶん（FF_LFN_BUF + 1 = 256 バイト）で済む。
 *
 * first が真なら prev を見ずに全体の最小を採る。want_dir でファイル / ディレクトリを
 * 切り替える。長すぎて飛ばした数は first のときだけ数える（2 周目以降は同じ物を見るため）。
 */
static bool scan_min_gt(walk_t *w, uint32_t depth, const char *prev, bool first,
                        bool want_dir, uint32_t *size_out, bool *io_err)
{
    DIR *dp = &s_dp[depth - 1u];
    size_t base = strlen(s_path);

    *io_err = false;
    if (f_opendir(dp, s_path) != FR_OK)
    {
        *io_err = true;
        return false;
    }

    bool found = false;
    uint32_t best_size = 0;

    for (;;)
    {
        if (f_readdir(dp, &s_fi) != FR_OK || s_fi.fname[0] == '\0')
        {
            break; /* 終端かエラー */
        }
        if (want_dir ? !is_walkable_dir(&s_fi) : !is_listable(&s_fi, w->exts, w->n_exts))
        {
            continue;
        }
        if (!fits(w, base, s_fi.fname))
        {
            if (first)
            {
                w->skip_long++;
            }
            continue;
        }
        if (!first && filelist_name_cmp(s_fi.fname, prev) <= 0)
        {
            continue; /* もう採った */
        }
        if (!found || filelist_name_cmp(s_fi.fname, s_best) < 0)
        {
            snprintf(s_best, sizeof(s_best), "%s", s_fi.fname);
            best_size = (uint32_t)s_fi.fsize;
            found = true;
        }
    }

    f_closedir(dp);

    if (size_out != NULL)
    {
        *size_out = best_size;
    }
    return found;
}

/* s_path のディレクトリにあるサブディレクトリの数。深さ上限で降りないときの報告用。 */
static uint32_t count_subdirs(uint32_t depth)
{
    DIR *dp = &s_dp[depth - 1u];

    if (f_opendir(dp, s_path) != FR_OK)
    {
        return 0;
    }

    uint32_t n = 0;
    for (;;)
    {
        if (f_readdir(dp, &s_fi) != FR_OK || s_fi.fname[0] == '\0')
        {
            break;
        }
        if (is_walkable_dir(&s_fi))
        {
            n++;
        }
    }

    f_closedir(dp);
    return n;
}

/* ---- 段ごとの処理 ------------------------------------------------------ */

static void walk(walk_t *w, uint32_t depth);

/* この段のファイルを名前の昇順で出す。 */
static void print_files(walk_t *w, uint32_t depth)
{
    size_t base = strlen(s_path);
    bool first = true;

    for (;;)
    {
        if (w->emitted >= w->max_entries)
        {
            w->truncated = true;
            break;
        }

        uint32_t size = 0;
        bool io_err = false;
        if (!scan_min_gt(w, depth, s_prev, first, false, &size, &io_err))
        {
            if (io_err)
            {
                printf("# warn    : %s: io error\n", s_path);
            }
            break;
        }

        /* サイズを先に置く。名前は空白を含みうるので必ず最後の欄にする。 */
        path_push(base, s_best);
        printf("# file    : %9u %s\n", (unsigned)size, &s_path[w->root_len + 1u]);
        s_path[base] = '\0';

        if (w->tick != NULL)
        {
            w->tick();
        }

        snprintf(s_prev, sizeof(s_prev), "%s", s_best);
        first = false;
        w->emitted++;
    }

    s_path[base] = '\0';
}

/* この段のファイルを buf へ詰める。並べ替えはこのディレクトリの範囲だけで閉じる。 */
static void collect_files(walk_t *w, uint32_t depth)
{
    DIR *dp = &s_dp[depth - 1u];
    size_t base = strlen(s_path);

    if (f_opendir(dp, s_path) != FR_OK)
    {
        printf("# warn    : %s: io error\n", s_path);
        return;
    }

    uint32_t from = w->buf->count;

    for (;;)
    {
        if (f_readdir(dp, &s_fi) != FR_OK || s_fi.fname[0] == '\0')
        {
            break; /* 終端かエラー */
        }
        if (!is_listable(&s_fi, w->exts, w->n_exts))
        {
            continue;
        }
        if (!fits(w, base, s_fi.fname))
        {
            w->skip_long++;
            continue;
        }

        path_push(base, s_fi.fname);
        bool ok = insert_sorted(w->buf, from, &s_path[w->root_len + 1u]);
        s_path[base] = '\0';

        if (!ok)
        {
            w->buf->truncated = true;
            w->truncated = true;
            break;
        }
    }

    f_closedir(dp);
    s_path[base] = '\0';
}

/* この段のサブディレクトリを名前の昇順で 1 つずつ降りる。 */
static void walk_subdirs(walk_t *w, uint32_t depth)
{
    size_t base = strlen(s_path);

    if (depth >= FILELIST_MAX_DEPTH)
    {
        w->skip_deep += count_subdirs(depth);
        return;
    }

    bool first = true;

    for (;;)
    {
        if (w->truncated)
        {
            break;
        }

        /*
         * 「直前に降りたディレクトリ名」は s_path の末尾要素そのもの。親を開き直す間だけ
         * 区切りを終端へ差し替える。prev はその後ろを指すので影響を受けない。
         */
        const char *prev = "";
        if (!first)
        {
            prev = &s_path[base + 1u];
            s_path[base] = '\0';
        }

        bool io_err = false;
        bool found = scan_min_gt(w, depth, prev, first, true, NULL, &io_err);
        if (!found)
        {
            if (io_err)
            {
                printf("# warn    : %s: io error\n", s_path);
            }
            break;
        }

        path_push(base, s_best);
        walk(w, depth + 1u);
        first = false;
    }

    s_path[base] = '\0';
}

/* 深さ優先。ファイルを出し切ってからサブディレクトリへ降りる。 */
static void walk(walk_t *w, uint32_t depth)
{
    if (w->buf != NULL)
    {
        collect_files(w, depth);
    }
    else
    {
        print_files(w, depth);
    }

    if (w->truncated)
    {
        return;
    }
    walk_subdirs(w, depth);
}

/*
 * 入口の共通処理。ストレージの状態とルートの存在を確かめ、walk_t と s_path を整える。
 *
 * ルート自身が開けないときだけ失敗にする（従来と同じ "not found" / "io error"）。
 * 途中のサブディレクトリが開けないときは警告を出して続ける。
 */
static const char *walk_begin(walk_t *w, const char *dir, const char *const *exts,
                              uint32_t n_exts, uint32_t path_max, filelist_buf_t *buf)
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

    size_t root_len = strlen(dir);
    if (root_len == 0u || root_len > FILELIST_ROOT_MAX)
    {
        return "bad argument";
    }

    memset(w, 0, sizeof(*w));
    w->exts = exts;
    w->n_exts = n_exts;
    w->path_max = (path_max < FILELIST_PATH_MAX) ? path_max : FILELIST_PATH_MAX;
    w->root_len = (uint32_t)root_len;
    w->buf = buf;

    snprintf(s_path, sizeof(s_path), "%s", dir);
    s_prev[0] = '\0';
    s_best[0] = '\0';

    DIR dp;
    FRESULT fr = f_opendir(&dp, s_path);
    if (fr == FR_NO_PATH || fr == FR_NO_FILE)
    {
        return "not found";
    }
    if (fr != FR_OK)
    {
        return "io error";
    }
    f_closedir(&dp);

    return NULL;
}

/* 走査中に飛ばしたものの報告。件数の行より前に出す。 */
static void walk_report(const walk_t *w)
{
    if (w->skip_long != 0u)
    {
        printf("# warn    : skipped %u path(s) longer than %u chars\n",
               (unsigned)w->skip_long, (unsigned)w->path_max);
    }
    if (w->skip_deep != 0u)
    {
        printf("# warn    : skipped %u directory(s) deeper than %u levels\n",
               (unsigned)w->skip_deep, (unsigned)FILELIST_MAX_DEPTH);
    }
}

/* ---- 入口 -------------------------------------------------------------- */

const char *filelist_print(const char *dir, const char *const *exts, uint32_t n_exts,
                           uint32_t max_entries, void (*tick)(void))
{
    walk_t w;
    const char *err = walk_begin(&w, dir, exts, n_exts, FILELIST_PATH_MAX, NULL);
    if (err != NULL)
    {
        return err;
    }

    w.max_entries = max_entries;
    w.tick = tick;

    walk(&w, 1u);

    walk_report(&w);
    if (w.truncated)
    {
        printf("# warn    : truncated at %u entries\n", (unsigned)max_entries);
    }
    printf("# files   : %u\n", (unsigned)w.emitted);
    return NULL;
}

const char *filelist_collect(const char *dir, const char *const *exts, uint32_t n_exts,
                             uint32_t path_max, filelist_buf_t *buf)
{
    walk_t w;
    const char *err = walk_begin(&w, dir, exts, n_exts, path_max, buf);
    if (err != NULL)
    {
        return err;
    }

    walk(&w, 1u);

    walk_report(&w);
    if (buf->truncated)
    {
        printf("# warn    : truncated at %u entries\n", (unsigned)buf->count);
    }

    return NULL;
}
