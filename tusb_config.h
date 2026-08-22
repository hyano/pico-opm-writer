/*
 * TinyUSB のデバイス設定
 *
 * アプリが tinyusb_device をリンクすると pico_stdio_usb は自前のディスクリプタを
 * 供給しなくなり（PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS が 0 になる）、
 * SDK 同梱の tusb_config.h（pico_stdio_usb 用）は
 * `#if !defined(LIB_TINYUSB_HOST) && !defined(LIB_TINYUSB_DEVICE)` の外側に出て中身が
 * 丸ごと無効化される。そのためこのファイルが tusb_config.h の実体になる。
 * CMakeLists.txt の target_include_directories が SDK の -isystem より優先されるため、
 * リポジトリ直下に置いたこのファイルが使われる。
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef _TUSB_CONFIG_H
#define _TUSB_CONFIG_H

/*
 * CFG_TUSB_MCU / CFG_TUSB_OS は tinyusb_device がリンクする
 * tinyusb_common_base（family_support.cmake）が target_compile_definitions で
 * 既に渡している（CFG_TUSB_MCU=OPT_MCU_RP2040 / CFG_TUSB_OS=OPT_OS_PICO）。
 * ここで再定義すると SDK 側の値と衝突するので触らない。
 */

/* デバイススタックを使う（ホストスタックは使わない） */
#define CFG_TUD_ENABLED 1

/*
 * ルートポート 0 はデバイスモード。引数なしの tusb_init() はこの定義を見て
 * 初期化するポートを決めるので、省略すると static assert で落ちる。
 */
#define CFG_TUSB_RHPORT0_MODE OPT_MODE_DEVICE

/* RP2350 の USB はフルスピードのみ */
#define CFG_TUD_MAX_SPEED OPT_MODE_FULL_SPEED

/* CDC を 2 本（#0 コマンド用 / #1 PCM データ出力用） */
#define CFG_TUD_CDC 2

/* コマンド応答・PCM データとも受信は使わないので既定の 256 バイトで足りる */
#define CFG_TUD_CDC_RX_BUFSIZE 256

/*
 * 送信バッファは全 CDC インスタンス共通の値。PCM 出力（CDC #1）が 250KB/s 程度を
 * 流す想定で、ホスト側の読み出しが一時的に遅れても取りこぼさないよう、
 * 既定の 64 バイトではなく 4096 バイトを確保する。
 */
#define CFG_TUD_CDC_TX_BUFSIZE 4096

/* フルスピードのバルクエンドポイントのパケットサイズ */
#define CFG_TUD_CDC_EP_BUFSIZE 64

/* 内蔵フラッシュ後半を PC へ見せるマスストレージ */
#define CFG_TUD_MSC 1

/*
 * MSC クラス層のバッファサイズ。バルク EP のパケットサイズ (64) とは別物で、
 * 論理セクタ長と同じ 512 にしておくと read10/write10 コールバックの offset が
 * 常に 0 になり、1 回の呼び出しがちょうど 1 セクタになる。
 * この define が無いと msc_device.h が #error になる。
 */
#define CFG_TUD_MSC_EP_BUFSIZE 512

/* 残りのクラスは使わない */
#define CFG_TUD_HID    0
#define CFG_TUD_MIDI   0
#define CFG_TUD_VENDOR 0

/*
 * USB DMA 用のメモリ配置属性。RP2350 に特別なメモリ領域制約は無いので、
 * SDK 同梱の tusb_config.h（pico_stdio_usb 用）と同じく空 / 4 バイトアラインでよい。
 */
#ifndef CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_SECTION
#endif

#ifndef CFG_TUSB_MEM_ALIGN
#define CFG_TUSB_MEM_ALIGN __attribute__((aligned(4)))
#endif

#endif
