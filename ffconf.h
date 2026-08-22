/*
 * FatFs R0.16 の設定
 *
 * external/fatfs/ の上流ソースには手を入れず、設定だけをこちら側に置く。
 *
 * この仕組みは「external/fatfs/ に ffconf.h が無いこと」に依存している。
 * ff.h の #include "ffconf.h" は、まず ff.h 自身のあるディレクトリを探すので、
 * 上流のテンプレートをそこへ置くとこのファイルが黙って無視される。
 * 事故を検出するため OPM_FFCONF_H を定義し、diskio_flash.c で #error 検査している。
 *
 * 項目は R0.16 のテンプレートに合わせて全部並べてある。版を上げるときは
 * 新しいテンプレートと突き合わせて増減を反映すること。
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef OPM_FFCONF_H
#define OPM_FFCONF_H

/* ff.h の FF_DEFINED と一致していること。ずれていれば ff.h がコンパイル時に弾く。 */
#define FFCONF_DEF 80386

/* ---- 機能の取捨 -------------------------------------------------------- */

/*
 * 書き込みを有効にするのは storage format（f_mkfs）のためだけ。
 * 通常運転で FatFs 側が書くことは無く、ファイルを置くのは PC が USB MSC 経由で行う。
 */
#define FF_FS_READONLY 0

/* f_getfree / f_mkdir / f_readdir を使うので削らない */
#define FF_FS_MINIMIZE 0

/*
 * f_findfirst / f_findnext は使わない。拡張子の判定は f_readdir の結果に対して
 * 自前で行う（LFN と SFN の両方に対してパターンが当たる曖昧さを持ち込まない）。
 */
#define FF_USE_FIND 0

#define FF_USE_MKFS 1 /* storage format で使う */

/*
 * VGM の読み出しは前進読みと、ループ先頭への seek だけ。CLMT のためのメモリと
 * 複雑さに見合わない。
 */
#define FF_USE_FASTSEEK 0

#define FF_USE_EXPAND  0
#define FF_USE_CHMOD   0
#define FF_USE_LABEL   1 /* format 時の f_setlabel と storage status の表示 */
#define FF_USE_FORWARD 0
#define FF_USE_STRFUNC 0 /* f_gets / f_printf は使わない */
#define FF_PRINT_LLI   0
#define FF_PRINT_FLOAT 0
#define FF_STRF_ENCODE 0

/* ---- 文字コードとファイル名 -------------------------------------------- */

/* SBCS。ffunicode.c からは 437 のテーブルだけが生成される。 */
#define FF_CODE_PAGE 437

/*
 * AFTERBURNER.VGM は 15 文字あり 8.3 に収まらないので LFN は必須。
 * 1 = 静的バッファ。3（ヒープ）は ff_memalloc を要求し ffsystem.c が必要になる。
 * 単一スレッドなので 1 で足りる。
 */
#define FF_USE_LFN     1
#define FF_MAX_LFN     255 /* 静的バッファは (255+1)*2 = 512 バイトだけ */
#define FF_LFN_UNICODE 0   /* TCHAR = char。ANSI/OEM のまま扱う */
#define FF_LFN_BUF     255
#define FF_SFN_BUF     12

/* ---- ドライブとボリューム ---------------------------------------------- */

#define FF_FS_RPATH   0  /* "/VGM/xxx" の絶対パスしか使わない */
#define FF_PATH_DEPTH 10 /* FF_FS_RPATH 0 なので効かない */

#define FF_VOLUMES         1
#define FF_STR_VOLUME_ID   0 /* "0:" の接頭辞を書かなくてよい */
#define FF_VOLUME_STRS     "RAM"
#define FF_MULTI_PARTITION 0 /* SFD（MBR 無し）で作るので不要 */

/* 論理セクタは 512 固定。MIN == MAX なので disk_ioctl(GET_SECTOR_SIZE) は呼ばれない。 */
#define FF_MIN_SS 512
#define FF_MAX_SS 512

#define FF_LBA64    0 /* 2MiB。32bit LBA で足りる */
#define FF_MIN_GPT  0x10000000
#define FF_USE_TRIM 0 /* 消去はフラッシュ側（flash_disk.c）で管理する */

/* ---- システム ---------------------------------------------------------- */

/* FIL ごとに 512 バイトの窓を持つ。RAM は余っている。 */
#define FF_FS_TINY 0

/* FAT12 だけ使う。exFAT を切るとコードもメモリも大きく減る。 */
#define FF_FS_EXFAT 0

/*
 * RTC が無いので固定の日時を使う。これにより get_fattime() を実装しなくて済む。
 */
#define FF_FS_NORTC   1
#define FF_NORTC_MON  1
#define FF_NORTC_MDAY 1
#define FF_NORTC_YEAR 2026
#define FF_FS_CRTIME  0

#define FF_FS_NOFSINFO 0 /* FAT32 用。FAT12 では効かない */

/* 単一スレッドで、同時に開くファイルは 1 個だけ。 */
#define FF_FS_LOCK 0

/* 単一コア・単一スレッドなので ff_mutex_* を実装しなくて済む。 */
#define FF_FS_REENTRANT 0
#define FF_FS_TIMEOUT   1000

#endif /* OPM_FFCONF_H */
