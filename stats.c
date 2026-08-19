/*
 * 実行時リソース使用量の計測の実装
 *
 * SPDX-License-Identifier: MIT
 */
#include "stats.h"

#include "pico/stdlib.h"

/* ---- 1 秒窓管理 --------------------------------------------------------- */

static struct {
    uint32_t window_start_us;    /* 現在の窓の開始時刻 [us] */
    uint32_t busy_us;            /* 窓内の累積実行時間 [us] */
    uint64_t frames_in_window;   /* 窓内で追加されたフレーム数 */
} s_window = {
    .window_start_us = 0,
    .busy_us = 0,
    .frames_in_window = 0,
};

/* ---- CPU 使用率 -------------------------------------------------------- */

static struct {
    uint32_t cpu_percent;     /* 直近窓の CPU 使用率 [%] */
    uint32_t cpu_percent_max; /* リセット以降の最大値 [%] */
} s_cpu = {
    .cpu_percent = 0,
    .cpu_percent_max = 0,
};

/* ---- DMA リング -------------------------------------------------------- */

static struct {
    uint32_t frames_current;  /* 現在の未処理フレーム数 */
    uint32_t frames_max;      /* リセット以降の最大値 */
} s_ring = {
    .frames_current = 0,
    .frames_max = 0,
};

/* ---- USB TX ------------------------------------------------------------ */

static struct {
    uint32_t bytes_current;   /* 現在の滞留バイト数 */
    uint32_t bytes_max;       /* リセット以降の最大値 */
} s_usb_tx = {
    .bytes_current = 0,
    .bytes_max = 0,
};

/* ---- I2S 出力 ---------------------------------------------------------- */

/*
 * こちらは減る方向が危険なので low-water を残す。UINT32_MAX は「まだ 1 度も
 * 通知されていない」ことを表す番兵。
 */
static struct {
    uint32_t depth_current;   /* 現在の先行フレーム数 */
    uint32_t depth_min;       /* リセット以降の最小値 */
} s_i2s = {
    .depth_current = 0,
    .depth_min = UINT32_MAX,
};

/* ---- カウンタ ---------------------------------------------------------- */

static struct {
    uint32_t overrun;      /* DMA overrun 発生回数 */
    uint64_t forbidden;    /* 禁止コード E=0 の検出数 */
    uint32_t rxstall;      /* PIO RX FIFO あふれ回数 */
    uint64_t frames;       /* 総フレーム数 */
    uint32_t frame_rate;   /* 直近窓でのフレームレート [frames/s] */
    uint32_t i2s_underrun; /* I2S の先行分が尽きた回数 */
} s_counters = {
    .overrun = 0,
    .forbidden = 0,
    .rxstall = 0,
    .frames = 0,
    .frame_rate = 0,
    .i2s_underrun = 0,
};

/* ---- フラッシュ書き込み / VGM ------------------------------------------ */

static struct {
    uint32_t flash_write;          /* 4KiB ブロックの書き出し回数 */
    uint32_t flash_blackout_max_us;/* 書き出しでメインループが止まった最大時間 */
    uint32_t seq_lag_max_us;       /* シーケンサの遅れの最大値 */
} s_ext = {
    .flash_write = 0,
    .flash_blackout_max_us = 0,
    .seq_lag_max_us = 0,
};

/* ---- CPU 使用率 -------------------------------------------------------- */

void stats_busy_add(uint32_t busy_us) {
    s_window.busy_us += busy_us;
}

void stats_service(void) {
    uint32_t now_us = time_us_32();
    uint32_t elapsed_us = now_us - s_window.window_start_us;

    /* 1 秒 (1000000us) に満たなければ何もしない */
    if (elapsed_us < 1000000u) {
        return;
    }

    /* 窓が閉じた。統計を計算する。 */

    /* CPU 使用率を計算。窓は 1 秒ちょうどとは限らないので実経過時間で割る。 */
    uint32_t cpu_pct = (uint32_t)(((uint64_t)s_window.busy_us * 100u) / elapsed_us);
    if (cpu_pct > 100u) {
        cpu_pct = 100u;
    }
    s_cpu.cpu_percent = cpu_pct;

    /* 最大値を更新 */
    if (cpu_pct > s_cpu.cpu_percent_max) {
        s_cpu.cpu_percent_max = cpu_pct;
    }

    /* フレームレートを計算（frames/s） */
    uint32_t frame_rate = (uint32_t)(
        (s_window.frames_in_window * 1000000u) / elapsed_us
    );
    s_counters.frame_rate = frame_rate;

    /* 次の窓へ */
    s_window.window_start_us = now_us;
    s_window.busy_us = 0;
    s_window.frames_in_window = 0;
}

uint32_t stats_cpu_percent(void) {
    return s_cpu.cpu_percent;
}

uint32_t stats_cpu_percent_max(void) {
    return s_cpu.cpu_percent_max;
}

/* ---- DMA リング -------------------------------------------------------- */

void stats_ring_update(uint32_t unread_frames) {
    s_ring.frames_current = unread_frames;
    if (unread_frames > s_ring.frames_max) {
        s_ring.frames_max = unread_frames;
    }
}

uint32_t stats_ring_frames(void) {
    return s_ring.frames_current;
}

uint32_t stats_ring_frames_max(void) {
    return s_ring.frames_max;
}

/* ---- USB TX ------------------------------------------------------------ */

void stats_usb_tx_update(uint32_t pending_bytes) {
    s_usb_tx.bytes_current = pending_bytes;
    if (pending_bytes > s_usb_tx.bytes_max) {
        s_usb_tx.bytes_max = pending_bytes;
    }
}

uint32_t stats_usb_tx_bytes(void) {
    return s_usb_tx.bytes_current;
}

uint32_t stats_usb_tx_bytes_max(void) {
    return s_usb_tx.bytes_max;
}

/* ---- I2S 出力 ---------------------------------------------------------- */

void stats_i2s_update(uint32_t depth_frames) {
    s_i2s.depth_current = depth_frames;
    if (depth_frames < s_i2s.depth_min) {
        s_i2s.depth_min = depth_frames;
    }
}

uint32_t stats_i2s_depth(void) {
    return s_i2s.depth_current;
}

uint32_t stats_i2s_depth_min(void) {
    return s_i2s.depth_min;
}

/* ---- カウンタ ---------------------------------------------------------- */

void stats_count_overrun(void) {
    s_counters.overrun++;
}

void stats_count_forbidden(uint32_t n) {
    s_counters.forbidden += n;
}

void stats_count_rxstall(void) {
    s_counters.rxstall++;
}

void stats_count_frames(uint32_t n) {
    s_counters.frames += n;
    s_window.frames_in_window += n;
}

void stats_count_i2s_underrun(void) {
    s_counters.i2s_underrun++;
}

uint32_t stats_overrun(void) {
    return s_counters.overrun;
}

uint64_t stats_forbidden(void) {
    return s_counters.forbidden;
}

uint32_t stats_rxstall(void) {
    return s_counters.rxstall;
}

uint64_t stats_frames(void) {
    return s_counters.frames;
}

uint32_t stats_i2s_underrun(void) {
    return s_counters.i2s_underrun;
}

uint32_t stats_frame_rate(void) {
    return s_counters.frame_rate;
}

/* ---- フラッシュ書き込み ------------------------------------------------ */

void stats_count_flash_write(void) {
    s_ext.flash_write++;
}

void stats_flash_blackout_add(uint32_t us) {
    if (us > s_ext.flash_blackout_max_us) {
        s_ext.flash_blackout_max_us = us;
    }
}

uint32_t stats_flash_write(void) {
    return s_ext.flash_write;
}

uint32_t stats_flash_blackout_max_us(void) {
    return s_ext.flash_blackout_max_us;
}

/* ---- シーケンサ再生 (VGM / MDX で共用) ---------------------------------- */

void stats_seq_lag_update(uint32_t us) {
    if (us > s_ext.seq_lag_max_us) {
        s_ext.seq_lag_max_us = us;
    }
}

uint32_t stats_seq_lag_max_us(void) {
    return s_ext.seq_lag_max_us;
}

/* ---- 初期化・リセット -------------------------------------------------- */

void stats_init(void) {
    s_window.window_start_us = time_us_32();
    s_window.busy_us = 0;
    s_window.frames_in_window = 0;

    s_cpu.cpu_percent = 0;
    s_cpu.cpu_percent_max = 0;

    s_ring.frames_current = 0;
    s_ring.frames_max = 0;

    s_usb_tx.bytes_current = 0;
    s_usb_tx.bytes_max = 0;

    s_i2s.depth_current = 0;
    s_i2s.depth_min = UINT32_MAX;

    s_counters.overrun = 0;
    s_counters.forbidden = 0;
    s_counters.rxstall = 0;
    s_counters.frames = 0;
    s_counters.frame_rate = 0;
    s_counters.i2s_underrun = 0;

    s_ext.flash_write = 0;
    s_ext.flash_blackout_max_us = 0;
    s_ext.seq_lag_max_us = 0;
}

void stats_reset(void) {
    /* high-water / low-water とカウンタを初期値に戻す */
    s_cpu.cpu_percent_max = 0;
    s_ring.frames_max = 0;
    s_usb_tx.bytes_max = 0;
    s_i2s.depth_min = UINT32_MAX;

    s_counters.overrun = 0;
    s_counters.forbidden = 0;
    s_counters.rxstall = 0;
    s_counters.frames = 0;
    s_counters.frame_rate = 0;
    s_counters.i2s_underrun = 0;

    s_ext.flash_write = 0;
    s_ext.flash_blackout_max_us = 0;
    s_ext.seq_lag_max_us = 0;

    /* 窓をリセット */
    s_window.window_start_us = time_us_32();
    s_window.busy_us = 0;
    s_window.frames_in_window = 0;
}
