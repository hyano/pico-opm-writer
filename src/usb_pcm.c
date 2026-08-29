/*
 * PCM 出力用 USB CDC (#1) の入出力の実装
 *
 * SPDX-License-Identifier: MIT
 */
#include "usb_pcm.h"

#include "tusb.h"

bool usb_pcm_connected(void)
{
    return tud_cdc_n_connected(USB_PCM_CDC_ITF);
}

uint32_t usb_pcm_writable(void)
{
    return tud_cdc_n_write_available(USB_PCM_CDC_ITF);
}

uint32_t usb_pcm_write(const void *buf, uint32_t len)
{
    return tud_cdc_n_write(USB_PCM_CDC_ITF, buf, len);
}

void usb_pcm_flush(void)
{
    tud_cdc_n_write_flush(USB_PCM_CDC_ITF);
}

uint32_t usb_pcm_pending(void)
{
    /*
     * TinyUSB は空き容量しか教えてくれないので、容量との差で滞留量を出す。
     * これはファーム内の送信 FIFO の滞留量であって、USB エンドポイントの状態ではない。
     */
    uint32_t avail = tud_cdc_n_write_available(USB_PCM_CDC_ITF);
    uint32_t cap = usb_pcm_capacity();
    return (avail >= cap) ? 0u : (cap - avail);
}

void usb_pcm_clear(void)
{
    tud_cdc_n_write_clear(USB_PCM_CDC_ITF);
}

uint32_t usb_pcm_capacity(void)
{
    return (uint32_t)CFG_TUD_CDC_TX_BUFSIZE;
}
