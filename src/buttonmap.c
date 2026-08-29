/*
 * ボタン起点の操作の実装
 *
 * SPDX-License-Identifier: MIT
 */
#include "buttonmap.h"

#include <stdio.h>

#include "autoplay.h"
#include "button.h"
#include "capture.h"
#include "led.h"
#include "mdx.h"
#include "sched.h"
#include "storage.h"
#include "vgm.h"

/*
 * 起動時にボタンで選んだ動作モードの結果。
 *
 * 起動直後の printf はホストが CDC を開く前なので捨てられる。後から USB を
 * 挿しても分かるよう、ここに控えて i の button 行で出す（組み立ては
 * buttonmap_boot_apply()、取り出しは buttonmap_boot_result()）。
 */
static char s_boot_result[48] = "none";

/*
 * ボタン起点の操作。
 *
 * 出すのは `#` で始まる情報行だけで、OK / ERR は出さない。コマンドの応答では
 * ないので、「1 コマンド 1 応答」（README §3.3）を崩さないため。失敗の理由は
 * 各 API の戻り値をそのまま埋める。直前に各モジュールの `# hint` / `# warn` が
 * 出るので、原因は 2 行セットで読める。
 */

/* 通知行に出すボタンの名前 */
static const char *button_name(uint32_t mask)
{
    if (mask == BUTTON_MASK_BOTH)
    {
        return "SW1+SW2";
    }
    if (mask == BUTTON_MASK_SW2)
    {
        return "SW2";
    }
    return "SW1";
}

/*
 * autoplay を指定の曲順で始め直す。storage host 中なら先に取り戻す。
 * 成功なら NULL、失敗ならエラー理由。
 */
static const char *button_start_autoplay(autoplay_mode_t mode)
{
    if (storage_mode() == STORAGE_MODE_HOST)
    {
        const char *err = storage_set_player();
        if (err != NULL)
        {
            return err;
        }
        printf("# storage : %s\n", storage_mode_name());
    }

    autoplay_set_mode(mode);

    /*
     * autoplay_start() はプレイリストを作り直して 1 曲目から始める。走っていた
     * 再生（手動の vgm play / mdx play を含む）はこの中で止まるので、ここで
     * 明示的に止める必要は無い。
     */
    return autoplay_start();
}

/*
 * ファイルシステムを PC へ渡す。
 *
 * autoplay / VGM / MDX は先に止める。キャプチャは止めない（`p 1` 中は必ず
 * ホストが CDC を握っていて `p 0` を打てるので、ボタンで黙って WAV を切る
 * 利益が無い）。残る拒否要因はキャプチャ中だけになり、storage_set_host() の
 * hint がそのまま理由になる。
 */
static const char *button_enter_host(void)
{
    autoplay_stop();
    if (vgm_is_playing())
    {
        vgm_stop();
    }
    if (mdx_is_playing())
    {
        mdx_stop();
    }
    return storage_set_host();
}

/* 長押し。元のモードを破棄して新しいモードへ移る。 */
static void button_do_long(uint32_t mask)
{
    const char *err;

    if (mask == BUTTON_MASK_BOTH)
    {
        printf("# button  : SW1+SW2 long: storage host\n");
        err = button_enter_host();
        if (err != NULL)
        {
            printf("# button  : storage host failed (%s)\n", err);
        }
        return;
    }

    bool shuffle = (mask == BUTTON_MASK_SW2);

    printf("# button  : %s long: autoplay %s\n", button_name(mask), shuffle ? "random" : "list");
    err = button_start_autoplay(shuffle ? AUTOPLAY_MODE_RANDOM : AUTOPLAY_MODE_LIST);
    if (err != NULL)
    {
        printf("# button  : autoplay start failed (%s)\n", err);
    }
}

/* 短押し。曲送り。停止中はそのボタンの曲順で始める。 */
static void button_do_short(uint32_t mask)
{
    const char *err;

    if (mask == BUTTON_MASK_BOTH)
    {
        /* storage host は破壊的なので、短押しでは絶対に起こさない */
        printf("# button  : SW1+SW2 short: ignored (hold 1 s for storage host)\n");
        return;
    }

    /*
     * HOST 中は短押しを効かせない。PC がマウントしたままディスクを引き抜くと
     * 書きかけのファイルが壊れるうえ、macOS では一度 eject すると USB を挿し
     * 直すまで再マウントできない。抜けるのは長押しか SW3 のリセットだけにする。
     */
    if (storage_mode() == STORAGE_MODE_HOST)
    {
        printf("# button  : %s short: ignored (storage is handed to the PC)\n",
               button_name(mask));
        return;
    }

    bool forward = (mask != BUTTON_MASK_SW2);

    if (autoplay_is_running())
    {
        printf("# button  : %s short: %s track\n", button_name(mask),
               forward ? "next" : "prev");
        err = autoplay_skip(forward ? 1 : -1);
        if (err != NULL)
        {
            printf("# button  : autoplay %s failed (%s)\n", forward ? "next" : "prev", err);
        }
        return;
    }

    /* 停止中は曲送りが成立しないので、そのボタンの曲順で始める */
    printf("# button  : %s short: autoplay %s\n", button_name(mask),
           forward ? "list" : "random");
    err = button_start_autoplay(forward ? AUTOPLAY_MODE_LIST : AUTOPLAY_MODE_RANDOM);
    if (err != NULL)
    {
        printf("# button  : autoplay start failed (%s)\n", err);
    }
}

/*
 * ボタンのイベントを 1 個消化する。
 *
 * **必ず main() の for(;;) 直下から呼ぶ。** service_all() の中から呼ぶと、
 * コマンド処理中の待ち（`d` の遅延、`p 0` のドレイン、一覧出力の tick）から
 * 再入して filelist の走査バッファを壊し、応答の途中へ別の出力が割り込む。
 *
 * 取り出しを実行より先に済ませてあるので、実行中に押されたぶんは次の周回へ回る。
 */
void buttonmap_dispatch(void)
{
    button_event_t ev;

    if (!button_take_event(&ev))
    {
        return;
    }

    if (ev.press == BUTTON_PRESS_LONG)
    {
        button_do_long(ev.mask);
    }
    else
    {
        button_do_short(ev.mask);
    }
}

/*
 * 起動時に押されていたボタンで動作モードを決め、両方が離されてから 1 回だけ
 * 実行する。押されていなければ何もしない（従来どおりの起動）。
 *
 * autoplay_start() は storage_init() のマウントを前提にするので、初期化列の
 * 最後（autoplay_init() の後）で呼ぶこと。
 */
void buttonmap_boot_apply(void)
{
    uint32_t chord = button_boot_chord();

    if (chord == 0u)
    {
        return;
    }

    /* 点滅の回数で選んだモードを示す。1 = list / 2 = random / 3 = storage host */
    led_boot_pattern(chord == BUTTON_MASK_SW1 ? 1u : (chord == BUTTON_MASK_SW2 ? 2u : 3u));
    button_boot_wait_release(service_all);
    led_set_state(LED_STATE_IDLE);

    const char *label;
    const char *err;

    if (chord == BUTTON_MASK_BOTH)
    {
        label = "storage host";
        err = button_enter_host();
    }
    else if (chord == BUTTON_MASK_SW2)
    {
        label = "autoplay random";
        err = button_start_autoplay(AUTOPLAY_MODE_RANDOM);
    }
    else
    {
        label = "autoplay list";
        err = button_start_autoplay(AUTOPLAY_MODE_LIST);
    }

    snprintf(s_boot_result, sizeof(s_boot_result), "%s (%s)", label,
             (err != NULL) ? err : "ok");

    /*
     * ここまでの printf はホストが CDC を開いていないと 1 本あたり最大
     * PICO_STDIO_USB_STDOUT_TIMEOUT_US (10ms) ブロックする。複数行出すと I2S の
     * 先行量 16.4ms を超え得るので、長く止まったあとの作法どおり張り直す。
     */
    capture_resync_after_blackout();
}

const char *buttonmap_boot_result(void)
{
    return s_boot_result;
}
