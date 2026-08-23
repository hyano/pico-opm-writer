/*
 * GPIO ボタン (SW1 = GP21 / SW2 = GP22) の取り込み
 *
 * スイッチのもう片側は GND で外付けプルアップは無い。RP2350 の内部プルアップを
 * 張って負論理（押下 = L）で読む。SW3 は RUN 端子に繋がっていてハードリセットが
 * かかるだけなので、ファームウェアからは見えない。
 *
 * **このモジュールは autoplay も storage も知らない。** 押下を取り込んで
 * 短押し / 長押しのイベントに変えるところまでで、何をするかは呼び出し側が決める。
 * service_all() がコマンド処理中からも再入的に呼ばれるため、ここから上位の操作を
 * 直接呼ぶと filelist の走査バッファを壊し、コマンド応答の途中に別の出力が
 * 割り込む。イベントは 1 個のメールボックスに置くだけにして、消化は
 * pico-opm-writer.c がメインループのトップレベルで行う。
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef BUTTON_H
#define BUTTON_H

#include <stdbool.h>
#include <stdint.h>

#ifndef BUTTON_ENABLED
#define BUTTON_ENABLED 1
#endif

/* ピン割り当て。README §1.1 の接続表と揃えること。 */
#define BUTTON_PIN_SW1 21u
#define BUTTON_PIN_SW2 22u

/* 押されているボタンの集合 */
#define BUTTON_MASK_SW1  0x1u
#define BUTTON_MASK_SW2  0x2u
#define BUTTON_MASK_BOTH (BUTTON_MASK_SW1 | BUTTON_MASK_SW2)

/*
 * デバウンスの窓。この時間だけ同じ値が続いたら確定値を更新する。
 *
 * 回数ではなく時間で測る。service_all() の呼ばれ方は状況で桁違いに変わる
 * （通常は数十万周/秒、フラッシュの書き出し中は数十 ms 空く）ので、
 * 「N 回連続一致」では時定数が定義できない。
 */
#ifndef BUTTON_DEBOUNCE_US
#define BUTTON_DEBOUNCE_US 10000u
#endif

/*
 * 長押しのしきい値。押し続けてこの時間に達した瞬間に長押しが成立する
 * （離すのを待たない）。
 */
#ifndef BUTTON_LONG_PRESS_US
#define BUTTON_LONG_PRESS_US 1000000u
#endif

typedef enum
{
    BUTTON_PRESS_SHORT = 0, /* しきい値未満で離した */
    BUTTON_PRESS_LONG,      /* しきい値に達した */
} button_press_t;

typedef struct
{
    button_press_t press;
    uint32_t mask; /* BUTTON_MASK_* の OR。押している間に押された全部 */
} button_event_t;

/* ---- 初期化とサービス -------------------------------------------------- */

/*
 * GPIO を入力 + プルアップにして、整定を待ってから起動時の押下を採取する。
 * 採取したぶんは実行時のイベントにはしない（起動モードで消化する）。
 *
 * main() の早い位置（led_init() の直後）で呼ぶ。プルアップを最初に効かせて
 * 整定時間を稼ぐためと、素早く離した利用者を取りこぼさないため。
 */
void button_init(void);

/* メインループから呼ぶ。デバウンスと短押し / 長押しの判定を進める。 */
void button_service(void);

/* ---- 起動時のモード選択 ------------------------------------------------ */

/* button_init() の時点で押されていた集合。0 なら通常起動。 */
uint32_t button_boot_chord(void);

/*
 * 起動時に押されていたボタンが全部離されるまで待つ。
 *
 * tick には service_all() を渡す。待っている間もメインループを回さないと、
 * キャプチャと I2S の DMA リング（一周 65.5ms）の位置を見失い、USB の列挙も
 * 進まない。抜けるときに状態機械を初期化するので、起動時の押下が実行時の
 * 短押しとして二重に発火することはない。
 */
void button_boot_wait_release(void (*tick)(void));

/* ---- イベント ---------------------------------------------------------- */

/*
 * メールボックスから 1 個取り出す。無ければ false。
 *
 * 深さは 1 で、埋まっている間に来たイベントは捨てる（古い方を残す）。
 * 捨てたことをその場で printf しないのは、button_service() が一覧出力の途中
 * からも呼ばれるため。数だけ button_dropped() に積む。
 */
bool button_take_event(button_event_t *out);

/* メールボックスが埋まっていて捨てたイベントの数（診断用） */
uint32_t button_dropped(void);

#endif /* BUTTON_H */
