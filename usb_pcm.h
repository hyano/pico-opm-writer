/*
 * PCM 出力用 USB CDC (#1) の入出力
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef USB_PCM_H
#define USB_PCM_H

#include <stdbool.h>
#include <stdint.h>

/* PCM を流す CDC のインスタンス番号。#0 はコマンド用 (stdio) が使っている。 */
#define USB_PCM_CDC_ITF 1u

/* ホストがポートを開いているか（DTR）。閉じていたら PCM は送らない。 */
bool usb_pcm_connected(void);

/* いま追加で書けるバイト数。0 なら送信バッファが満杯。 */
uint32_t usb_pcm_writable(void);

/*
 * len バイトを送信バッファへ積む。実際に積めたバイト数を返す。
 * 空きが無くてもブロックしない。呼ぶ前に usb_pcm_writable() で上限を取ること。
 */
uint32_t usb_pcm_write(const void *buf, uint32_t len);

/* 送信バッファをエンドポイントへ押し出す */
void usb_pcm_flush(void);

/* 送信バッファに滞留しているバイト数（エンドポイントの状態ではない） */
uint32_t usb_pcm_pending(void);

/* 送信バッファの中身を捨てる。切断・再開時に古いデータを残さないために使う。 */
void usb_pcm_clear(void);

/* 送信バッファの容量 [bytes]。統計表示の分母に使う。 */
uint32_t usb_pcm_capacity(void);

#endif /* USB_PCM_H */
