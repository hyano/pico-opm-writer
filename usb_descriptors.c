/*
 * USB ディスクリプタ（CDC 2 本構成）
 *
 * pico_stdio_usb はアプリが tinyusb_device を直接リンクすると自前のディスクリプタを
 * 供給しなくなる（PICO_STDIO_USB_USE_DEFAULT_DESCRIPTORS が 0 になり、SDK 同梱の
 * stdio_usb_descriptors.c は #if で丸ごと無効化される）ため、このファイルで
 * tud_descriptor_device_cb() / tud_descriptor_configuration_cb() /
 * tud_descriptor_string_cb() を提供する。
 *
 * CDC #0（ITF 0/1）= 既存のコマンド用。stdio_usb がそのまま itf 0 を使うので、
 * CDC #0 を先頭に置くことで macOS 上のデバイス名（.../cu.usbmodem<location>01）が
 * 変わらないようにしている。
 * CDC #1（ITF 2/3）= PCM データ出力用。アプリが tud_cdc_n_write(1, ...) で書く。
 *
 * SPDX-License-Identifier: MIT
 */
#include "pico/unique_id.h"
#include "tusb.h"

/* ---- デバイスディスクリプタ ------------------------------------------------ */

/*
 * VID/PID は SDK の pico_stdio_usb（stdio_usb_descriptors.c）の既定値をそのまま踏襲する。
 * VID = Raspberry Pi、PID は RP2040 と RP2040 以外（RP2350 など）で値が異なる。
 * このリポジトリは PICO_BOARD=pico2（RP2350）固定なので実際には 0x0009 が使われる。
 */
#define USBD_VID (0x2E8A) /* Raspberry Pi */
#if PICO_RP2040
#define USBD_PID (0x000a) /* Raspberry Pi Pico SDK CDC for RP2040 */
#else
#define USBD_PID (0x0009) /* Raspberry Pi Pico SDK CDC */
#endif

#define USBD_MANUFACTURER "Raspberry Pi"
#define USBD_PRODUCT "Pico"

/* IAD（Interface Association Descriptor）構成であることを示すクラス/サブクラス/プロトコル */
static const tusb_desc_device_t s_usbd_desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = USBD_VID,
    .idProduct = USBD_PID,
    .bcdDevice = 0x0100,
    .iManufacturer = 1,
    .iProduct = 2,
    .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

const uint8_t *tud_descriptor_device_cb(void) {
    return (const uint8_t *)&s_usbd_desc_device;
}

/* ---- コンフィグレーションディスクリプタ ------------------------------------ */

/* インタフェース番号（CDC は control/data の 2 つを消費する） */
enum {
    USBD_ITF_CDC0 = 0, /* コマンド用 CDC の control インタフェース */
    USBD_ITF_CDC0_DATA,
    USBD_ITF_CDC1, /* PCM 用 CDC の control インタフェース */
    USBD_ITF_CDC1_DATA,
    USBD_ITF_MAX,
};

/* エンドポイントアドレス（notification は IN 割り込み、data は OUT/IN バルク） */
#define USBD_CDC0_EP_NOTIF (0x81)
#define USBD_CDC0_EP_OUT (0x02)
#define USBD_CDC0_EP_IN (0x82)
#define USBD_CDC1_EP_NOTIF (0x83)
#define USBD_CDC1_EP_OUT (0x04)
#define USBD_CDC1_EP_IN (0x84)

/* notification エンドポイントの最大パケットサイズ / data エンドポイントはフルスピードの 64 バイト */
#define USBD_CDC_NOTIF_MAX_SIZE (8)
#define USBD_CDC_DATA_MAX_SIZE (64)

/* 文字列ディスクリプタのインデックス（0 は言語 ID 用に予約） */
enum {
    USBD_STR_MANUF = 1,
    USBD_STR_PRODUCT,
    USBD_STR_SERIAL,
    USBD_STR_CDC0,
    USBD_STR_CDC1,
};

#define USBD_DESC_LEN (TUD_CONFIG_DESC_LEN + 2 * TUD_CDC_DESC_LEN)

static const uint8_t s_usbd_desc_cfg[USBD_DESC_LEN] = {
    /* コンフィグ番号, インタフェース数, 文字列インデックス, 総バイト長, 属性, 消費電流(mA) */
    TUD_CONFIG_DESCRIPTOR(1, USBD_ITF_MAX, 0, USBD_DESC_LEN, 0, 250),

    /*
     * TUD_CDC_DESCRIPTOR(control インタフェース番号, 名前の文字列インデックス,
     *                     notification EP, notification EP サイズ,
     *                     data OUT EP, data IN EP, data EP サイズ)
     * data インタフェース番号は control の直後（control + 1）に自動的に割り当てられる。
     */
    TUD_CDC_DESCRIPTOR(USBD_ITF_CDC0, USBD_STR_CDC0, USBD_CDC0_EP_NOTIF,
        USBD_CDC_NOTIF_MAX_SIZE, USBD_CDC0_EP_OUT, USBD_CDC0_EP_IN, USBD_CDC_DATA_MAX_SIZE),

    TUD_CDC_DESCRIPTOR(USBD_ITF_CDC1, USBD_STR_CDC1, USBD_CDC1_EP_NOTIF,
        USBD_CDC_NOTIF_MAX_SIZE, USBD_CDC1_EP_OUT, USBD_CDC1_EP_IN, USBD_CDC_DATA_MAX_SIZE),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index; /* コンフィグは 1 つしか無いので無視する */
    return s_usbd_desc_cfg;
}

/* ---- 文字列ディスクリプタ -------------------------------------------------- */

/* フラッシュのユニーク ID から生成するシリアル番号（16進文字列 + 終端) */
static char s_usbd_serial_str[PICO_UNIQUE_BOARD_ID_SIZE_BYTES * 2 + 1];

/* CDC #0/#1 それぞれに名前を付け、ホスト側でどちらのポートか判別できるようにする */
static const char *const s_usbd_desc_str[] = {
    [USBD_STR_MANUF] = USBD_MANUFACTURER,
    [USBD_STR_PRODUCT] = USBD_PRODUCT,
    [USBD_STR_SERIAL] = s_usbd_serial_str,
    [USBD_STR_CDC0] = "pico-opm-writer command",
    [USBD_STR_CDC1] = "pico-opm-writer PCM",
};

/* UTF-16 変換後の文字列バッファ（終端の長さ+種別ワードを含む）の最大ワード数 */
#define USBD_DESC_STR_MAX (32)

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;

    static uint16_t s_desc_str[USBD_DESC_STR_MAX];

    /* フラッシュのユニーク ID からシリアル番号を初回だけ生成する */
    if (!s_usbd_serial_str[0]) {
        pico_get_unique_board_id_string(s_usbd_serial_str, sizeof(s_usbd_serial_str));
    }

    uint8_t len;
    if (index == 0) {
        /* インデックス 0 は対応言語一覧（英語のみ） */
        s_desc_str[1] = 0x0409;
        len = 1;
    } else {
        if (index >= sizeof(s_usbd_desc_str) / sizeof(s_usbd_desc_str[0])) {
            return NULL;
        }
        const char *str = s_usbd_desc_str[index];
        for (len = 0; len < USBD_DESC_STR_MAX - 1 && str[len]; ++len) {
            s_desc_str[1 + len] = (uint16_t)str[len];
        }
    }

    /* 先頭 1 ワードは (長さ<<8 | ディスクリプタ種別)、以降が UTF-16LE 本体 */
    s_desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));

    return s_desc_str;
}
