/*
 * メインループの協調スケジューラの実装
 *
 * 各サービスを**毎周回ではなく、グループごとに最短間隔を決めて**回す。
 * 間隔と決め手は下の *_INTERVAL_US に置いてある（README §3.11.1 にも同じ表がある）。
 *
 * SPDX-License-Identifier: MIT
 */
#include "sched.h"

#include "pico/stdlib.h"
#include "tusb.h"

#include "autoplay.h"
#include "button.h"
#include "capture.h"
#include "i2s.h"
#include "led.h"
#include "mdx.h"
#include "pcm8.h"
#include "songend.h"
#include "stats.h"
#include "storage.h"
#include "vgm.h"
#include "ym3012.h"

/*
 * 音声チェーン（リング位置の取り込み / ADPCM / キャプチャ送出 / I2S 供給）を
 * 回す最短間隔。
 *
 * フレームは φM/64 = 62500/s で届くので、毎周回回すと 1 回あたり 1 フレーム
 * 未満のために毎回ブロックの支度をすることになり、
 * 固定費だけで CPU の大半を使う（pcm8.c の PCM8_BATCH_FRAMES と同じ理屈を
 * チェーン全体へ広げたもの）。どのリングも一周 65.5ms あるので、この間隔なら
 * DMA の位置を見失わない。
 *
 * 大きくするほど固定費は下がるが、1 回で埋める量が増えて I2S の先行量の谷が
 * 深くなる。STATS_PROFILE=1 で測った実測（BOS14.MDX + USB キャプチャ、
 * サービス滞在時間の合計 us/s）:
 *
 *   間隔なし 620,718 / 100us 206,435 / 250us 160,238 / 500us 135,048 / 1000us 120,566
 *
 * 250us 以降は逓減し、1000us では SEQ LAG が 220us まで伸びる。500us を採る。
 * このとき 1 回で扱うのは 31 フレームで、I2S の先行 1024 フレームに対して十分小さい。
 */
#ifndef AUDIO_SERVICE_INTERVAL_US
#define AUDIO_SERVICE_INTERVAL_US 500u
#endif

/*
 * tud_task() を回す最短間隔。
 *
 * これも毎周回の固定費なので、音声チェーンだけを間引くとループが速く回るように
 * なったぶん呼び出し回数が増え、浮いた時間をそのまま食う（実測: 音声チェーンを
 * 250us 間隔にしたら LOOP が 3.4 倍になり USB の占有率が 15% -> 26% へ上がった）。
 *
 * **下限は PCM 出力のパケットレートで決まる。** tud_task() 1 回で CDC の
 * バルクエンドポイントへ載るのは EP バッファ 1 個ぶん (CFG_TUD_CDC_EP_BUFSIZE
 * = 64 バイト) なので、
 *
 *   下限 = 64 バイト / (phiM/64 x 4 バイト) = 64 / 250000 = 256us   (phiM 4MHz)
 *
 * を超えるとホストへ送り出す速さが取り込む速さに追いつかず、CDC #1 の送信 FIFO が
 * 詰まってキャプチャのリングが 65.5ms で overrun する。実測でも 500us にすると
 * `p 1` の直後に STATE が ERROR へ落ちた（250us でも USB_TX が 3968/4096 まで
 * 埋まって余裕が無い）。2 倍の余裕を見て 125us にしてある。
 *
 * phiM を下げるとレートも下がるので制約は緩む方向。上げる場合はここを見直すこと。
 */
#ifndef USB_TASK_INTERVAL_US
#define USB_TASK_INTERVAL_US 125u
#endif

/*
 * シーケンサ (VGM / MDX) の予定時刻を確かめる最短間隔。
 *
 * これも毎周回では多すぎる。MDX の 1 tick は最短でも 4ms 台、VGM の wait は
 * 最短 1 サンプル = 22.7us で、どちらも予算ループの中で遅れを取り返すので、
 * この間隔ぶんの遅れは `s` の SEQ LAG に乗るだけで発行そのものは詰まらない。
 * 音声チェーンより短くして、遅れが目に見えて増えないようにしてある。
 */
#ifndef SEQ_SERVICE_INTERVAL_US
#define SEQ_SERVICE_INTERVAL_US 50u
#endif

/*
 * ストレージの書き出しを確かめる最短間隔。
 *
 * 書き出しの判断は STORAGE_FLUSH_IDLE_MS (250ms) のアイドル期限なので、
 * 1ms 刻みで見れば足りる。HOST モードでなければ即座に返る。
 */
#ifndef STORAGE_SERVICE_INTERVAL_US
#define STORAGE_SERVICE_INTERVAL_US 1000u
#endif

/*
 * ボタン (GP21 / GP22) を読む最短間隔。
 *
 * デバウンスの窓 10ms に対して 10 サンプル取れて十分細かく、1 秒の長押し判定に
 * 対しては誤差 0.1% で十分粗い。人間には 1ms の遅れは見えない。
 */
#ifndef BUTTON_SERVICE_INTERVAL_US
#define BUTTON_SERVICE_INTERVAL_US 1000u
#endif

/* 各グループを最後に回した時刻。上の *_INTERVAL_US の基準。 */
static uint32_t s_usb_last_us;
static uint32_t s_audio_last_us;
static uint32_t s_seq_last_us;
static uint32_t s_storage_last_us;
static uint32_t s_button_last_us;

/*
 * service_all() を最後に呼んだ時刻と、基準が揃ったかどうか。
 *
 * リアルタイムの余裕を決めるのは「サービスが最大どれだけ空いたか」なので、
 * その間隔を stats へ渡す（stats.h の「メインループの周期」）。**起動から
 * 最初の 1 回は渡さない。** 上の基準時刻はどれも 0 から始まるため、1 回目の
 * 差分は間隔ではなく起動からの経過時間になり、high-water を汚す。
 */
static uint32_t s_loop_mark_us;
static bool s_loop_marked;

/*
 * 前回から interval_us 以上たっていたら true を返し、基準時刻を進める。
 *
 * 「最短でもこの間隔を空ける」であって「この周期ちょうどで回す」ではない。
 * メインループが長く止まったあとは 1 回だけ真になり、取りこぼしを溜めない。
 */
static bool due(uint32_t *last_us, uint32_t now_us, uint32_t interval_us)
{
    if ((uint32_t)(now_us - *last_us) < interval_us)
    {
        return false;
    }
    *last_us = now_us;
    return true;
}

void service_all(void)
{
    uint32_t t0 = time_us_32();

    if (s_loop_marked)
    {
        stats_loop_period_add(t0 - s_loop_mark_us);
    }
    s_loop_mark_us = t0;

    if (due(&s_usb_last_us, t0, USB_TASK_INTERVAL_US))
    {
        tud_task();
    }

    uint32_t t1 = time_us_32();

    /*
     * 音声チェーンは AUDIO_SERVICE_INTERVAL_US ごとにまとめて回す。
     *
     * 書き込み位置の取り込みと ADPCM のレンダリングは、どの消費者よりも先に
     * 行う。capture も I2S も ym3012_reader_read_pcm() の中でミックス済みの
     * PCM を受け取るので、そこまでに描き終わっていなければならない。
     * 間隔を空けても**この順序は変えない**。
     */
    uint32_t audio_prev_us = s_audio_last_us;
    if (due(&s_audio_last_us, t1, AUDIO_SERVICE_INTERVAL_US))
    {
        if (s_loop_marked)
        {
            stats_audio_gap_add(t1 - audio_prev_us);
        }

        ym3012_ring_poll();
        STATS_SVC(STATS_SVC_PCM8, pcm8_service());

        STATS_SVC(STATS_SVC_CAPTURE, capture_service());
        STATS_SVC(STATS_SVC_I2S, i2s_service());
    }

    /*
     * シーケンサは音声チェーンより短い間隔で見る。予定時刻の判定そのものは軽いが、
     * 毎周回だと周回数ぶんの固定費がそのまま乗る。
     */
    if (due(&s_seq_last_us, t1, SEQ_SERVICE_INTERVAL_US))
    {
        STATS_SVC(STATS_SVC_VGM, vgm_service());
        STATS_SVC(STATS_SVC_MDX, mdx_service());
        /*
         * 曲の終わり方と曲送りはシーケンサの後。同じ周回で更新された再生状態を
         * そのまま見られる。songend は autoplay より前（autoplay は songend が
         * 出した結論を見て曲を送る）。判定は比較が数回で済むので STATS_SVC では
         * 包まない。
         */
        songend_service();
        autoplay_service();
    }

    /*
     * 演奏連動キャプチャ（`p 2`）へ「曲が続いているか」を渡す。比較が数回で済むので
     * due() では間引かない。**待機から録り始めるまでの遅れをメインループ 1 周に
     * 抑える**ためで、ここを間引くと WAV の先頭に曲の頭が欠ける。
     */
    capture_note_track(songend_is_active(), songend_track_seq());

    if (due(&s_storage_last_us, t1, STORAGE_SERVICE_INTERVAL_US))
    {
        STATS_SVC(STATS_SVC_STORAGE, storage_service());
    }

    /*
     * ボタンは取り込むだけで、autoplay / storage は触らない。この関数はコマンド
     * 処理中からも再入的に呼ばれるので、ここから上位の操作を呼ぶと filelist の
     * 走査バッファを壊す。消化は main() のトップレベルの button_dispatch()。
     *
     * led_service() の直前に置いてあるのは、長押しが成立した周回のうちに
     * LED の合図を反映させるため。判定は比較が数回で済むので STATS_SVC では
     * 包まない（led_service() / autoplay_service() と同じ扱い）。
     */
    if (due(&s_button_last_us, t1, BUTTON_SERVICE_INTERVAL_US))
    {
        button_service();
    }

    led_service();

    stats_usb_busy_add(t1 - t0);
    stats_busy_add(time_us_32() - t1);
    stats_service();

    /* ここまで来れば全グループの基準時刻が入っている。次回から差分を取る。 */
    s_loop_marked = true;
}
