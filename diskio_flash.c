/*
 * FatFs の disk I/O を内蔵フラッシュのブロックデバイスへ繋ぐ
 *
 * 上流の diskio.c は配布物ではなくサンプルなので取り込まず、ここで実装する。
 *
 * SPDX-License-Identifier: MIT
 */
/* diskio.h は ff.h の型（BYTE / LBA_t など）に依存するので順序を入れ替えないこと */
#include "ff.h"

#include "diskio.h"

#include "flash_disk.h"
#include "storage.h"

/*
 * external/fatfs/ に上流のテンプレート ffconf.h が置かれると、ff.h の
 * #include "ffconf.h" が自分の隣にあるそれを先に拾い、リポジトリ直下の設定が
 * 黙って無視される。ここで検出する。
 */
#ifndef OPM_FFCONF_H
#error "FatFs が external/fatfs/ffconf.h を拾っている。上流のテンプレートを置いてはいけない。"
#endif

/* 論理セクタ長は FatFs と flash_disk で一致していること */
_Static_assert(FF_MAX_SS == FLASH_DISK_SS, "FF_MAX_SS と FLASH_DISK_SS が食い違っている");
_Static_assert(FF_MIN_SS == FF_MAX_SS, "論理セクタ長は固定にすること");

/* ---- 状態 -------------------------------------------------------------- */

DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0)
    {
        return STA_NOINIT;
    }

    /*
     * HOST モード中はフラッシュの持ち主が PC（USB MSC）なので、FatFs 側からも
     * 触れないようにしておく。storage 側で f_unmount しているうえの二重の防御。
     */
    if (!storage_fatfs_may_access())
    {
        return STA_NOINIT;
    }

    return 0;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    return disk_status(pdrv);
}

/* ---- 読み書き ---------------------------------------------------------- */

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0)
    {
        return RES_PARERR;
    }
    if (!storage_fatfs_may_access())
    {
        return RES_NOTRDY;
    }
    if (!flash_disk_read((uint32_t)sector, buff, (uint32_t)count))
    {
        return RES_PARERR;
    }
    return RES_OK;
}

#if !FF_FS_READONLY
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0)
    {
        return RES_PARERR;
    }
    if (!storage_fatfs_may_access())
    {
        return RES_NOTRDY;
    }

    /*
     * ここへ来るのは storage format（f_mkfs / f_mkdir / f_setlabel）からだけ。
     * キャッシュが埋まったら書き出してから再試行する。つまりこの関数はブロックする。
     *
     * service_all() は呼ばない。storage_service() を再入することになるうえ、
     * フォーマットは利用者が明示的に叩いたコマンドなので待たせてよい。
     * 数回の I2S アンダーランは避けられない（README に記載）。
     */
    for (;;)
    {
        int rc = flash_disk_write((uint32_t)sector, buff, (uint32_t)count);
        if (rc == FLASH_DISK_OK)
        {
            return RES_OK;
        }
        if (rc != FLASH_DISK_BUSY)
        {
            return RES_PARERR;
        }
        if (!flash_disk_flush_one())
        {
            return RES_ERROR; /* 書き出せない = フラッシュ側の異常 */
        }
    }
}
#endif

/* ---- 制御 -------------------------------------------------------------- */

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0)
    {
        return RES_PARERR;
    }

    switch (cmd)
    {
    case CTRL_SYNC:
        return flash_disk_flush_all() ? RES_OK : RES_ERROR;

    case GET_SECTOR_COUNT:
        *(LBA_t *)buff = (LBA_t)FLASH_DISK_LBA_COUNT;
        return RES_OK;

    /*
     * 消去単位を論理セクタ数で返す。f_mkfs はこれを見てデータ領域を消去単位の
     * 境界へ揃えるので、クラスタ N がフラッシュの消去セクタ N にちょうど乗る。
     * この一致が崩れると 1 クラスタの書き換えに 2 回の消去が要るようになる。
     */
    case GET_BLOCK_SIZE:
        *(DWORD *)buff = (DWORD)FLASH_DISK_LBA_PER_ES;
        return RES_OK;

    /* GET_SECTOR_SIZE は FF_MIN_SS == FF_MAX_SS なので呼ばれない */
    default:
        return RES_PARERR;
    }
}
