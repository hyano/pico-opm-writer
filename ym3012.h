/*
 * YM3012 (OPM の DAC) 出力のキャプチャと PCM 変換
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef YM3012_H
#define YM3012_H

#include <stdbool.h>
#include <stdint.h>

/* ---- ピン割り当て ------------------------------------------------------ */

/* SO / φ1 / SH1 / SH2 は PIO の in_base から連続していること */
#define YM3012_PIN_SO   17 /* シリアルデータ */
#define YM3012_PIN_PHI1 18 /* ビットクロック（φM/2） */
#define YM3012_PIN_SH1  19 /* CH2 のサンプルホールド。フレーム同期に使う */
#define YM3012_PIN_SH2  20 /* CH1 のサンプルホールド。現在は未使用 */

/*
 * ループバック自己診断で使うピン。将来 I2S に割り当てる予定の 3 本を、
 * 起動時の一瞬だけ CPU から叩いて合成フレームを流し込む。
 * ここに何かを接続する場合は YM3012_LOOPBACK_ENABLED を 0 にする。
 */
#ifndef YM3012_LOOPBACK_ENABLED
#define YM3012_LOOPBACK_ENABLED 1
#endif
#ifndef YM3012_LOOPBACK_BASE
#define YM3012_LOOPBACK_BASE 26 /* +0=SO / +1=φ1 / +2=SH1 */
#endif

/* ---- リング ------------------------------------------------------------ */

/*
 * DMA のハードウェアリング機能を使うのでサイズは 2 の冪。
 * 1 フレーム (L+R) がちょうど 1 エントリ (32bit) なので、
 * 16KB = 4096 フレーム = 62500Hz で 65.5ms 分になる。
 */
#define YM3012_RING_BITS   14u
#define YM3012_RING_BYTES  (1u << YM3012_RING_BITS)
#define YM3012_RING_FRAMES (YM3012_RING_BYTES / 4u)

/* ---- PCM 変換 ---------------------------------------------------------- */

/*
 * YM3012 の 16bit ワード 1 個を signed 16bit PCM へ変換する。
 *
 * ワードの中身は受信順に「無効 3bit / 仮数 D0..D9 / 指数 S0..S2」。PIO が右シフトで
 * 詰めるので、bit0..2 が無効、bit3..12 が仮数、bit13..15 が指数になる。
 *
 * データシート LSI-2130123 (1992-04) より:
 *   - 仮数は 10bit オフセットバイナリ。符号付き値は m - 512（範囲 -512..+511）
 *   - 指数は E = S2*4 + S1*2 + S0。E が大きいほどステップが粗い（E=1 が最も細かい）
 *   - 線形値は pcm = (m - 512) << (E - 1)
 *   - E = 0 は 3→7 デコーダの禁止コードで正常な出力には現れない。0 として扱う
 *
 * 値域は -32768 (m=0, E=7) 〜 +32704 (m=1023, E=7 = 511<<6)。
 * 正側が +32767 に届かないのは仕様どおりで、正規化してはいけない。
 */
static inline int16_t ym3012_word_to_pcm(uint16_t w) {
    uint32_t m = ((uint32_t)w >> 3) & 0x3ffu;
    uint32_t e = ((uint32_t)w >> 13) & 0x7u;

    if (e == 0u) {
        return 0;
    }

    /*
     * 負値の左シフトは未定義動作なので符号なしでシフトしてから型を戻す。
     * 結果は必ず -32768..+32704 に収まるので、切り捨ては起きない。
     */
    uint32_t v = ((uint32_t)m - 512u) << (e - 1u);
    return (int16_t)(uint16_t)v;
}

/* 1 フレーム (32bit) を L / R へ分解する。下位 16bit が L、上位 16bit が R。 */
static inline void ym3012_frame_to_pcm(uint32_t frame, int16_t *l, int16_t *r) {
    *l = ym3012_word_to_pcm((uint16_t)frame);
    *r = ym3012_word_to_pcm((uint16_t)(frame >> 16));
}

/* ---- 初期化 ------------------------------------------------------------ */

/*
 * PIO と DMA を起動する。以後キャプチャは常時動作し、停止しない。
 * 内部でループバック自己診断を 1 回実施する（DMA 開始前）。
 */
void ym3012_init(void);

bool ym3012_selftest_passed(void);       /* ループバック自己診断の結果 */
const char *ym3012_selftest_detail(void); /* 失敗時の内訳。成功なら "PASS" */

/* ---- リング操作 -------------------------------------------------------- */

/*
 * DMA の書き込み位置を読み、内部の総フレーム数を更新する。
 * リングが一周する 65.5ms より十分短い間隔で呼ぶこと（メインループから毎周回）。
 */
void ym3012_ring_poll(void);

/* 未処理フレーム数。YM3012_RING_FRAMES 以上なら overrun。 */
uint32_t ym3012_unread(void);

/* 読み出し位置を現在の書き込み位置へ合わせる（未処理を 0 にする） */
void ym3012_ring_sync(void);

/* DMA が書いた総フレーム数。p 0 のドレイン範囲を決めるスナップショットに使う。 */
uint64_t ym3012_write_total(void);

/* CPU が処理した総フレーム数 */
uint64_t ym3012_read_total(void);

/*
 * 未処理フレームを最大 max_frames 個 PCM へ変換して out へ書く。
 * out は int16_t が 2 * max_frames 個入る大きさが必要。
 * 戻り値は実際に変換したフレーム数で、リング末尾をまたぐ場合はそこで切る。
 * 読み出し位置は変換した分だけ進む。
 */
uint32_t ym3012_read_pcm(int16_t *out, uint32_t max_frames);

/* PIO の RX FIFO あふれを検出してクリアする。あふれていたら true。 */
bool ym3012_check_rxstall(void);

/* ---- 自己テスト -------------------------------------------------------- */

/*
 * PCM 変換の単体テスト。既知のワードに対する期待値を突き合わせる。
 * 失敗したら detail に最初の不一致の内容を書いて false を返す。
 */
bool ym3012_pcm_selftest(const char **detail);

#endif /* YM3012_H */
