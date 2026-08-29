/*
 * 内蔵 QSPI フラッシュのブロックデバイスの実装
 *
 * SPDX-License-Identifier: MIT
 */
#include "flash_disk.h"

#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/flash.h"

#include "capture.h"
#include "stats.h"

/* 領域は末尾の予約セクタの手前まで収まり、消去単位で割り切れること */
_Static_assert(FLASH_FATFS_OFFSET < PICO_FLASH_SIZE_BYTES - FLASH_FATFS_TAIL_RESERVE,
               "FatFs 領域の開始位置がフラッシュの外に出ている");
_Static_assert(FLASH_FATFS_OFFSET + FLASH_FATFS_SIZE <= PICO_FLASH_SIZE_BYTES - FLASH_FATFS_TAIL_RESERVE,
               "FatFs 領域が末尾の予約セクタ（RP2350-E10）に掛かっている");
_Static_assert(FLASH_FATFS_OFFSET % FLASH_SECTOR_SIZE == 0,
               "FatFs 領域の開始位置は消去単位に揃えること");
_Static_assert(FLASH_FATFS_SIZE % FLASH_SECTOR_SIZE == 0,
               "FatFs 領域のサイズは消去単位の倍数にすること");
_Static_assert(FLASH_FATFS_TAIL_RESERVE % FLASH_SECTOR_SIZE == 0,
               "末尾の予約量は消去単位の倍数にすること");
_Static_assert(FLASH_DISK_ES % FLASH_PAGE_SIZE == 0,
               "消去単位は書き込みページの倍数であること");
/* クラスタ長は消去単位に固定なので、クラスタ数が FAT12 の上限を超えると
   f_mkfs が FAT16 を選び、クラスタと消去セクタの 1:1 対応が壊れる */
_Static_assert(FLASH_FATFS_SIZE / FLASH_DISK_ES < 4085u,
               "クラスタ数が FAT12 の上限を超える");

/* ---- キャッシュ -------------------------------------------------------- */

/*
 * 行のデータ部は別配列にしてある。flash_range_program() へ渡すポインタは
 * RAM を指している必要があり（XIP は書き込み中に止まる）、かつワード境界に
 * 揃えておきたいため。.bss なので条件を満たす。
 */
static uint8_t s_data[FLASH_DISK_CACHE_LINES][FLASH_DISK_ES] __attribute__((aligned(4)));

static struct
{
    uint32_t es_index; /* 担当する 4KiB ブロック番号 */
    uint32_t lru;      /* 単調増加のアクセス通し番号。小さいほど古い */
    bool valid;
    bool dirty;
} s_meta[FLASH_DISK_CACHE_LINES];

static uint32_t s_lru_clock;
static bool s_failed;

/* ---- XIP 直読み -------------------------------------------------------- */

static const uint8_t *xip_at(uint32_t offset_in_region)
{
    return (const uint8_t *)(XIP_BASE + FLASH_FATFS_OFFSET + offset_in_region);
}

/* ---- 行の割り当て ------------------------------------------------------ */

static int find_line(uint32_t es_index)
{
    for (uint32_t i = 0; i < FLASH_DISK_CACHE_LINES; i++)
    {
        if (s_meta[i].valid && s_meta[i].es_index == es_index)
        {
            return (int)i;
        }
    }
    return -1;
}

/*
 * es_index を担当する行を用意する。無ければ空き行か clean な最古行を使い、
 * XIP から 4KiB 読み込んで満たす。全行が valid かつ追い出す相手が dirty なら
 * -1（呼び出し側は BUSY として扱い、書き出し後に再試行する）。
 */
static int acquire_line(uint32_t es_index)
{
    int idx = find_line(es_index);
    if (idx >= 0)
    {
        s_meta[idx].lru = ++s_lru_clock;
        return idx;
    }

    /* 空き行を探す */
    int victim = -1;
    for (uint32_t i = 0; i < FLASH_DISK_CACHE_LINES; i++)
    {
        if (!s_meta[i].valid)
        {
            victim = (int)i;
            break;
        }
    }

    /* 無ければ clean な行のうち最も古いものを再利用する */
    if (victim < 0)
    {
        uint32_t oldest = UINT32_MAX;
        for (uint32_t i = 0; i < FLASH_DISK_CACHE_LINES; i++)
        {
            if (!s_meta[i].dirty && s_meta[i].lru < oldest)
            {
                oldest = s_meta[i].lru;
                victim = (int)i;
            }
        }
    }

    if (victim < 0)
    {
        return -1; /* 全行 dirty。書き出すまで待つ */
    }

    memcpy(s_data[victim], xip_at(es_index * FLASH_DISK_ES), FLASH_DISK_ES);
    s_meta[victim].es_index = es_index;
    s_meta[victim].lru = ++s_lru_clock;
    s_meta[victim].valid = true;
    s_meta[victim].dirty = false;
    return victim;
}

/* ---- 初期化 ------------------------------------------------------------ */

void flash_disk_init(void)
{
    memset(s_meta, 0, sizeof(s_meta));
    s_lru_clock = 0;
    s_failed = false;
}

void flash_disk_invalidate(void)
{
    memset(s_meta, 0, sizeof(s_meta));
    s_lru_clock = 0;
}

bool flash_disk_failed(void)
{
    return s_failed;
}

/* ---- 読み書き ---------------------------------------------------------- */

bool flash_disk_read(uint32_t lba, void *dst, uint32_t count)
{
    if (count == 0u || lba > FLASH_DISK_LBA_COUNT ||
        count > FLASH_DISK_LBA_COUNT - lba)
    {
        return false;
    }

    uint8_t *out = (uint8_t *)dst;

    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t l = lba + i;
        uint32_t es = l / FLASH_DISK_LBA_PER_ES;
        uint32_t off = (l % FLASH_DISK_LBA_PER_ES) * FLASH_DISK_SS;

        /*
         * clean な行の内容はフラッシュと同じなので、dirty のときだけ行から読む。
         * それ以外は XIP から直接読む（キャッシュを汚さない）。
         */
        int idx = find_line(es);
        if (idx >= 0 && s_meta[idx].dirty)
        {
            memcpy(out, &s_data[idx][off], FLASH_DISK_SS);
        }
        else
        {
            memcpy(out, xip_at(es * FLASH_DISK_ES + off), FLASH_DISK_SS);
        }
        out += FLASH_DISK_SS;
    }

    return true;
}

int flash_disk_write(uint32_t lba, const void *src, uint32_t count)
{
    if (count == 0u || lba > FLASH_DISK_LBA_COUNT ||
        count > FLASH_DISK_LBA_COUNT - lba)
    {
        return FLASH_DISK_ERR;
    }

    const uint8_t *in = (const uint8_t *)src;

    for (uint32_t i = 0; i < count; i++)
    {
        uint32_t l = lba + i;
        uint32_t es = l / FLASH_DISK_LBA_PER_ES;
        uint32_t off = (l % FLASH_DISK_LBA_PER_ES) * FLASH_DISK_SS;

        int idx = acquire_line(es);
        if (idx < 0)
        {
            return FLASH_DISK_BUSY;
        }

        memcpy(&s_data[idx][off], in, FLASH_DISK_SS);
        s_meta[idx].dirty = true;
        in += FLASH_DISK_SS;
    }

    return FLASH_DISK_OK;
}

/* ---- フラッシュへの書き出し -------------------------------------------- */

uint32_t flash_disk_dirty_lines(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < FLASH_DISK_CACHE_LINES; i++)
    {
        if (s_meta[i].valid && s_meta[i].dirty)
        {
            n++;
        }
    }
    return n;
}

/* 消去済み（全 0xFF）なら消去を省ける。新品基板の初回書き込みが速くなる。 */
static bool region_is_erased(uint32_t offset_in_region)
{
    const uint32_t *p = (const uint32_t *)(const void *)xip_at(offset_in_region);
    for (uint32_t i = 0; i < FLASH_DISK_ES / 4u; i++)
    {
        if (p[i] != 0xffffffffu)
        {
            return false;
        }
    }
    return true;
}

/* flash_safe_execute() から IRQ を落とした状態で呼ばれる */
typedef struct
{
    uint32_t offset;     /* フラッシュ先頭からのオフセット */
    const uint8_t *data; /* RAM 上の 4KiB */
    bool need_erase;
} flush_arg_t;

static void do_flush(void *param)
{
    const flush_arg_t *a = (const flush_arg_t *)param;

    if (a->need_erase)
    {
        flash_range_erase(a->offset, FLASH_DISK_ES);
    }
    flash_range_program(a->offset, a->data, FLASH_DISK_ES);
}

bool flash_disk_flush_one(void)
{
    /* 最も古い dirty 行を選ぶ */
    int idx = -1;
    uint32_t oldest = UINT32_MAX;
    for (uint32_t i = 0; i < FLASH_DISK_CACHE_LINES; i++)
    {
        if (s_meta[i].valid && s_meta[i].dirty && s_meta[i].lru < oldest)
        {
            oldest = s_meta[i].lru;
            idx = (int)i;
        }
    }
    if (idx < 0)
    {
        return false;
    }

    uint32_t region_off = (uint32_t)s_meta[idx].es_index * FLASH_DISK_ES;

    /*
     * 未使用ブロック（全 0xFF）なら消去を飛ばせる。新品基板では領域全体が
     * まるごと消去済みなので、初回のフォーマットと最初のコピーが目に見えて速くなる。
     */
    flush_arg_t arg = {
        .offset = FLASH_FATFS_OFFSET + region_off,
        .data = s_data[idx],
        .need_erase = !region_is_erased(region_off),
    };

    uint32_t t0 = time_us_32();
    int rc = flash_safe_execute(do_flush, &arg, 1000);
    uint32_t elapsed = time_us_32() - t0;

    /*
     * リング一周 65.5ms を超えて止まった可能性がある。ym3012 と I2S は
     * DMA ポインタの差分をリング長で剰余を取って積んでいるので、ここで
     * 基準点を張り直さないと総フレーム数に恒久的なずれが残る。
     */
    capture_resync_after_blackout();

    stats_count_flash_write();
    stats_flash_blackout_add(elapsed);

    if (rc != PICO_OK)
    {
        s_failed = true;
        return false;
    }

    s_meta[idx].dirty = false;
    return true;
}

bool flash_disk_flush_all(void)
{
    bool ok = true;
    while (flash_disk_dirty_lines() > 0u)
    {
        if (!flash_disk_flush_one())
        {
            ok = false;
            break;
        }
    }
    return ok;
}
