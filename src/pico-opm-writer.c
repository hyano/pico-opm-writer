/*
 * pico-opm-writer
 *
 * USB CDC のテキストコマンドから YM2151 (OPM) のレジスタを書き込む。
 *
 * このファイルが持つのは初期化列とメインループの骨格だけ。
 *
 *   sched.c     1 周分のサービス（service_all）
 *   buttonmap.c ボタン起点の操作
 *   console.c   コマンドの受け口
 *   report.c    i / s / h の状態表示
 *
 * SPDX-License-Identifier: MIT
 */
#include "hardware/clocks.h"
#include "pico/stdlib.h"
#include "tusb.h"

#include "autoplay.h"
#include "button.h"
#include "buttonmap.h"
#include "capture.h"
#include "clockmode.h"
#include "console.h"
#include "i2s.h"
#include "led.h"
#include "mdx.h"
#include "opm.h"
#include "pcm8.h"
#include "sched.h"
#include "songend.h"
#include "stats.h"
#include "storage.h"
#include "vgm.h"
#include "ym3012.h"

int main(void)
{
    /*
     * φM を整数分周で作るため、stdio 初期化より前にシステムクロックを上げる。
     * ここで決まるのは起動時のプリセットだけで、以後は clockmode.c が張り替える。
     */
    set_sys_clock_khz(OPM_SYS_CLOCK_KHZ, true);

    /*
     * CDC を 2 本にした都合で TinyUSB の初期化はアプリの責務になっている。
     * stdio_usb_init() は tud_inited() を assert するので、必ず先に呼ぶ。
     */
    tusb_init();
    stdio_init_all();

    led_init();

    /*
     * ボタンの GPIO。プルアップを最も早く効かせて整定時間を稼ぐため、また
     * 押されているかの採取をここで済ませて素早く離した利用者を取りこぼさない
     * ため、初期化列の先頭に置く。GP21 / GP22 は opm_init() の
     * gpio_init_mask()（GP2-GP14）にも ym3012 にも I2S にも含まれない。
     */
    button_init();

    stats_init();
    opm_init();

    /* PIO と DMA を起動する。以後キャプチャ経路は止めない。 */
    ym3012_init();
    capture_init();

    /* ADPCM のミキサ。ym3012 の総フレーム数を起点にするので ym3012_init() の後。 */
    pcm8_init();

    /*
     * I2S 出力。φM の分周比を使うので opm_init() より後、
     * GP26-GP28 を握るのでループバック自己診断を含む ym3012_init() より後に置く。
     */
    i2s_init();

    /* φM の実行時切り替え。現在のプリセットを控えるだけなので i2s_init() の後。 */
    clockmode_init();

    /* 内蔵フラッシュ上のファイルシステム。領域を検査して PLAYER でマウントする。 */
    storage_init();
    vgm_init();
    mdx_init();
    /* 曲の終わり方。vgm/mdx の状態を見るだけなので init の順はこの後ろでよい。 */
    songend_init();
    autoplay_init();

    /*
     * 起動時にボタンが押されていたら、離されるのを待ってからその動作モードで
     * 始める。autoplay_start() が storage のマウントを前提にするので、
     * 初期化列を全部終えたここで呼ぶ。
     */
    buttonmap_boot_apply();

    /*
     * メインループ。**この 4 行の順序が全体の構造そのもの。**
     *
     * ボタンの消化を service_all() の中ではなくここに置いてあるのが肝で、
     * ここが process_line() の外側であることが構文的に保証される唯一の点
     * （理由は buttonmap.h）。
     */
    for (;;)
    {
        service_all();          /* USB・音声・シーケンサ・ストレージをひと回し */
        buttonmap_dispatch();   /* ボタンのイベントを 1 個消化する */
        console_poll_connect(); /* ホストが CDC #0 を開いたか（100ms ごと） */
        console_service();      /* コマンドを 1 文字読む */
    }
}
