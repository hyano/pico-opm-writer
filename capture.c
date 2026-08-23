/*
 * PCM キャプチャの状態機械の実装
 *
 * SPDX-License-Identifier: MIT
 */
#include "capture.h"

#include <stdio.h>

#include "i2s.h"
#include "led.h"
#include "pcm8.h"
#include "stats.h"
#include "usb_pcm.h"
#include "ym3012.h"

/* 1 回の送信で扱うフレーム数の上限。大きくしすぎるとコマンド応答が鈍る。 */
#define CAPTURE_CHUNK_FRAMES 256u

/* 1 フレーム = L と R の int16 で 4 バイト */
#define CAPTURE_FRAME_BYTES 4u

static capture_state_t s_state;

/* p 0 を受けた時点の書き込み総フレーム数。ここまで送ったらドレイン完了。 */
static uint64_t s_drain_target;

/* stats へフレーム数を渡すときの前回値 */
static uint64_t s_counted_total;

/* 変換結果の一時置き場。L/R 交互に詰める。 */
static int16_t s_pcm[CAPTURE_CHUNK_FRAMES * 2u];

void capture_init(void)
{
    s_state = CAPTURE_STATE_IDLE;
    s_drain_target = 0;
    s_counted_total = 0;
    led_set_state(LED_STATE_IDLE);
}

capture_state_t capture_state(void)
{
    return s_state;
}

const char *capture_state_name(void)
{
    switch (s_state)
    {
    case CAPTURE_STATE_IDLE:
        return "IDLE";
    case CAPTURE_STATE_CAPTURING:
        return "CAPTURING";
    case CAPTURE_STATE_DRAINING:
        return "DRAINING";
    case CAPTURE_STATE_ERROR:
        return "ERROR";
    default:
        return "?";
    }
}

const char *capture_start(void)
{
    if (s_state == CAPTURE_STATE_CAPTURING || s_state == CAPTURE_STATE_DRAINING)
    {
        printf("# hint    : already capturing; run p 0 first\n");
        return "wrong state";
    }

    /*
     * 送り先が開いていないまま溜め込むと、リング 65.5ms 分で必ず overrun する。
     * コマンド段階のエラーとして返す（ファームの ERROR 状態にはしない）。
     */
    if (!usb_pcm_connected())
    {
        return "not connected";
    }

    /* p 1 以降に DMA が書いたデータだけを送る */
    ym3012_ring_poll();
    ym3012_ring_sync();
    usb_pcm_clear();

    s_state = CAPTURE_STATE_CAPTURING;
    led_set_state(LED_STATE_CAPTURE);
    return NULL;
}

/*
 * p 0。「確実に止める」コマンドなので、止まっている状態から呼んでも成功にする
 * （storage host / clock と同じ冪等の扱い）。DMA overrun で ERROR に落ちた場合も
 * ここで IDLE へ戻せるようにしてあり、p 1 を経由せずに復帰できる。
 */
const char *capture_request_stop(void)
{
    if (s_state == CAPTURE_STATE_IDLE)
    {
        return NULL;
    }
    if (s_state == CAPTURE_STATE_ERROR)
    {
        capture_abort(); /* LED のエラー表示もここで消える */
        return NULL;
    }
    if (s_state != CAPTURE_STATE_CAPTURING)
    {
        return NULL; /* DRAINING。既に停止処理に入っている */
    }

    /* この時点までに DMA が取り込んだ分を最後の 1 フレームまで送り切る */
    ym3012_ring_poll();
    s_drain_target = ym3012_write_total();
    s_state = CAPTURE_STATE_DRAINING;
    return NULL;
}

void capture_resync_after_blackout(void)
{
    ym3012_ring_resync();
    i2s_resync();
    pcm8_resync();
}

void capture_abort(void)
{
    s_state = CAPTURE_STATE_IDLE;
    ym3012_ring_sync();
    led_set_state(LED_STATE_IDLE);
}

/* 送信をやめて IDLE へ。CDC #1 の切断はエラーではないのでこちらを使う。 */
static void capture_to_idle(void)
{
    s_state = CAPTURE_STATE_IDLE;
    ym3012_ring_sync();
    led_set_state(LED_STATE_IDLE);
}

bool capture_service(void)
{
    /*
     * 書き込み位置の取り込み (ym3012_ring_poll) は service_all() が周回の先頭で
     * 済ませている。ADPCM のミキサがどの消費者よりも先にレンダリングする必要が
     * あり、そのためには poll がさらに前に来ていなければならない。
     */

    /*
     * 取り込んだフレーム数は状態によらず数える。RATE (frames/s) が
     * 期待値 φM/64 から外れていないかは、キャプチャしていなくても見たい。
     */
    uint64_t total = ym3012_write_total();
    if (total != s_counted_total)
    {
        stats_count_frames((uint32_t)(total - s_counted_total));
        s_counted_total = total;
    }

    /* RX FIFO があふれると L/R の並びが崩れるので、必ず拾っておく */
    if (ym3012_check_rxstall())
    {
        stats_count_rxstall();
    }

    uint32_t unread = ym3012_unread();
    stats_ring_update(unread);
    stats_usb_tx_update(usb_pcm_pending());

    /* 送らない状態では読み捨てて、未処理を溜めない（誤 overrun を防ぐ） */
    if (s_state == CAPTURE_STATE_IDLE || s_state == CAPTURE_STATE_ERROR)
    {
        ym3012_ring_sync();
        return false;
    }

    /* CDC #1 が閉じたら即停止。再接続しても自動では再開しない。 */
    if (!usb_pcm_connected())
    {
        capture_to_idle();
        return true;
    }

    /*
     * 未処理がリング全体に達したら、最も古いフレームが DMA に踏まれた可能性がある。
     * 壊れたデータを PCM として送り続けないよう、ここで打ち切る。
     */
    if (unread >= YM3012_RING_FRAMES)
    {
        s_state = CAPTURE_STATE_ERROR;
        stats_count_overrun();
        ym3012_ring_sync();
        led_set_state(LED_STATE_ERROR);
        /*
         * コマンド CDC へ通知する。ただし裸の `ERR ...` を非同期に出すと
         * 「1 コマンド 1 応答」の規約を破り、ホスト側のパーサが実行中のコマンドの
         * 応答と取り違えて同期を失う。`#` 始まりの情報行なら既存のホストツールが
         * 読み飛ばすので、規約を保ったまま通知できる。
         * 状態そのものは `s` の state と OVERRUN からも読める。
         */
        printf("# ERR dma overrun\n");
        return true;
    }

    uint32_t budget = unread;

    if (s_state == CAPTURE_STATE_DRAINING)
    {
        /*
         * ドレイン中は p 0 時点までの分しか送らない。
         *
         * ドレイン中にフラッシュの書き出しが入ると capture_resync_after_blackout() が
         * カーソルを現在位置へ寄せるので、read_total が s_drain_target を追い越しうる。
         * 素で引くと u64 が回り込んで budget が下がらず、完了枝に落ちなくなる。
         */
        uint64_t read_total = ym3012_read_total();
        uint64_t remain = (s_drain_target > read_total) ? (s_drain_target - read_total) : 0u;
        if (remain < (uint64_t)budget)
        {
            budget = (uint32_t)remain;
        }
        if (budget == 0u)
        {
            usb_pcm_flush();
            capture_to_idle();
            return true;
        }
    }

    if (budget == 0u)
    {
        return false;
    }

    /* 送信バッファに入る分しか変換しない。入らなければ次の周回に回す。 */
    uint32_t writable = usb_pcm_writable() / CAPTURE_FRAME_BYTES;
    if (writable == 0u)
    {
        usb_pcm_flush();
        return false; /* USB 待ち。CPU は何もしていないので idle 扱い */
    }
    if (budget > writable)
    {
        budget = writable;
    }
    if (budget > CAPTURE_CHUNK_FRAMES)
    {
        budget = CAPTURE_CHUNK_FRAMES;
    }

    uint32_t n = ym3012_read_pcm(s_pcm, budget);
    if (n == 0u)
    {
        return false;
    }

    /* 直前に空きを確認しているので、ここは必ず全部積める */
    (void)usb_pcm_write(s_pcm, n * CAPTURE_FRAME_BYTES);
    usb_pcm_flush();
    return true;
}
