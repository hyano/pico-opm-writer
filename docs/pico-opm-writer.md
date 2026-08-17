# pico-opm-writer（ファームウェア内部設計）

[pico-opm-writer.c](../pico-opm-writer.c) ほかリポジトリ直下の `.c` / `.h` / `.pio`

Raspberry Pi Pico 2 (RP2350) 上のファームウェアが、OPM のバス制御と YM3012 の
DAC キャプチャをどう実装しているかの説明。**使い方・配線・コマンド仕様は
[README.md](../README.md)** にある。

## 1. ソース構成

| ファイル | 役割 |
| --- | --- |
| [pico-opm-writer.c](../pico-opm-writer.c) | `main()`、システムクロック設定、初期化、メインループ、行入力とコマンドパーサ、応答出力 |
| [opm.h](../opm.h) / [opm.c](../opm.c) | OPM のピン割り当て・タイミング定数・φM プリセット（`OPM_CLOCK_MODE`）、バス制御 |
| [opm_clock.pio](../opm_clock.pio) | φM 生成用 PIO プログラムと `opm_clock_program_init()` |
| [ym3012.h](../ym3012.h) / [ym3012.c](../ym3012.c) | YM3012 の PIO + DMA リング初期化、リング位置の管理、PCM 変換、自己診断 |
| [ym3012.pio](../ym3012.pio) | キャプチャ用 PIO プログラムと `ym3012_capture_program_init()` |
| [capture.h](../capture.h) / [capture.c](../capture.c) | キャプチャの状態機械と、リング → PCM → CDC #1 の送出 |
| [i2s.h](../i2s.h) / [i2s.c](../i2s.c) | I2S 出力の PIO + DMA リング初期化、先行量の維持、アンダーラン復帰 |
| [i2s.pio](../i2s.pio) | I2S 出力用 PIO プログラムと `i2s_out_program_init()` |
| [usb_pcm.h](../usb_pcm.h) / [usb_pcm.c](../usb_pcm.c) | CDC #1 の接続判定・書き込み・滞留量 |
| [led.h](../led.h) / [led.c](../led.c) | 非ブロッキングな LED パターン表示 |
| [stats.h](../stats.h) / [stats.c](../stats.c) | CPU 使用率・high-water・カウンタ |
| [tusb_config.h](../tusb_config.h) / [usb_descriptors.c](../usb_descriptors.c) | USB CDC 2 本構成の TinyUSB 設定とディスクリプタ |

### 1.1 メインループ

メインループは `tud_task()` → キャプチャの送出 → I2S への供給 → LED → 統計 →
コマンド 1 文字読み、を回すだけ。`d` の待機と `p 0` のドレイン待ちからも同じ処理を
呼ぶので、コマンドの待ち時間の中でも PCM の送出・I2S への供給・USB の処理は止まらない。

**この周回は 2 つの DMA リングの位置を追いかけている**（キャプチャ側とI2S 側）。
どちらもリング一周は 65.5ms なので、1 周回がこれを超えると位置を見失う。

コマンド側の入出力はすべて標準入出力 API 経由で行う。

- 出力: `printf()` / `puts()`
- 入力: `getchar_timeout_us()` でノンブロッキングに 1 文字ずつ受け取り、行バッファに溜める
- 接続検出: `stdio_usb_connected()`

パーサ側の上限は [pico-opm-writer.c](../pico-opm-writer.c) に定義がある。1 行の最大長が
`LINE_MAX_LEN`（255 文字）、`d` に指定できる待機時間の上限が `DELAY_MAX_MS`（60000 ms）。

### 1.2 主要 API

```c
void opm_init(void);                        // GPIO と PIO(φM) を初期化し /IC リセットを実行
void opm_write(uint8_t addr, uint8_t data); // 1 レジスタ書き込み（アドレス→データの 2 サイクル）
void opm_reset(void);                       // /IC によるハードウェアリセット
void opm_clear(void);                       // ソフトウェアによる全レジスタクリア

uint32_t opm_clock_hz_actual(void);         // 実際に生成されている φM 周波数
uint32_t opm_clock_div_int(void);           // PIO 分周比の整数部
uint32_t opm_clock_div_frac(void);          // PIO 分周比の小数部（/256）
```

クロック照会の 3 関数は `i` の情報表示（[README §3.6](../README.md#36-i情報表示の出力例)）に使う。

## 2. φM の生成

PIO ステートマシン 1 基で GPIO をトグルする（1 命令 1 サイクル × 2 で 1 周期）。

```
φM = sys_clk / (2 × clkdiv)
```

プリセットの一覧と切り替え方は [README §2](../README.md#2-クロック設定-φm) にある。

### 2.1 システムクロックを φM に合わせる理由

φM とシステムクロックは**整数分周（ジッタなし）になる組**でしか意味を持たないため、
両者をペアにしたプリセットとして [opm.h](../opm.h) に持たせている。

`φM × 偶数 = sys_clk` がちょうど成り立つ組でのみ `clkdiv` が整数（小数部 0）になり、
ジッタのない φM が得られる。RP2350 の既定値 150MHz のままだと 4MHz は
`clkdiv = 18.75`、3.579545MHz は `clkdiv = 20.953125`（1/256 に丸めた値）となり、
どちらも小数分周になる。小数分周でも平均周波数の誤差は無視できる
（前者は誤差なし、後者で約 -35ppm）が、1 周期ごとに ±1 sys_clk
（150MHz で ±6.7ns、φM 3.58MHz の周期に対して約 2.4%）のジッタが乗る。

起動時に `set_sys_clock_khz(OPM_SYS_CLOCK_KHZ, true)` を **`stdio_init_all()` より前に**
呼んでいる。USB は独立した USB PLL から給電されるため、この変更の影響を受けない。
既定の 144MHz は RP2350 の定格 150MHz 以内なのでオーバークロックにあたらない。
`OPM_CLOCK_MODE_NTSC` の 157.5MHz は定格に対して約 5% のオーバークロックになるが、
電圧設定の変更なしで動作する範囲。

`OPM_CLOCK_HZ` / `OPM_SYS_CLOCK_KHZ` をプリセット外の値へ直接書き換えることもできるが、
その場合は **必ず両方を整数分周になる組で**指定すること（片方だけの変更は小数分周になる）。

### 2.2 小数分周へのフォールバック

プリセット外の φM を指定して整数分周にならない場合は、PIO の小数分周
（`sm_config_set_clkdiv_int_frac8()`、分解能 1/256）にフォールバックする。起動時の
情報表示で実際の分周比を確認できる。

## 3. OPM バス書き込みシーケンス

OPM への 1 レジスタ書き込みは「アドレスラッチ」→「データ書き込み」の 2 サイクルからなる。

```
[アドレスサイクル]
  A0 = L, D0-D7 = レジスタアドレス
  データ確定を待つ (t_SETUP)
  /WR = L → t_WR 保持 → /WR = H
  t_ADDR 待機

[データサイクル]
  A0 = H, D0-D7 = 書き込む値
  データ確定を待つ (t_SETUP)
  /WR = L → t_WR 保持 → /WR = H
  t_DATA 待機   ← OPM の内部処理（BUSY）が終わるまで
```

/RD を接続していないためステータスレジスタの BUSY フラグは読めない。代わりに
**データサイクル後に固定時間待つ**ことで代替している。

### 3.1 タイミング定数

[opm.h](../opm.h) の定数で調整する。クロック依存の待ち時間は `clock_get_hz(clk_sys)` から
実行時に算出しているため、φM プリセットを変えても定数の書き換えは要らない。

| 定数 | 既定値 | 考え方 |
| --- | --- | --- |
| `OPM_T_SETUP_NS` | 100 ns | データ確定から /WR 立ち下がりまで。実際は GPIO 操作の命令実行時間で満たされるが明示的に確保する |
| `OPM_T_WR_US` | 1 µs | /WR の L 期間 |
| `OPM_T_ADDR_US` | 5 µs | アドレスラッチ後の待機 |
| `OPM_T_DATA_US` | 25 µs | データ書き込み後の BUSY 待ち。68 φM サイクル ≒ 17µs（4MHz）/ ≒19µs（3.579545MHz）に余裕を持たせた値 |
| `OPM_T_IC_LOW_MS` | 10 ms | /IC の L 保持時間 |
| `OPM_T_IC_WAIT_MS` | 10 ms | /IC を H に戻してから最初の書き込みまでの待機 |

**`OPM_T_DATA_US` 以外はデータシートの最小値から詰めた値ではなく、余裕を大きく取った
仮置きである。** この値で [test/](../test/) の全キャプチャ（数千条件）が取れているので
「短すぎはしない」ことは分かっているが、どこまで短縮できるかは測っていない（未確認）。

既定値では 1 レジスタ書き込みに約 32µs かかる（最大 3 万回/秒）。USB CDC 経由の
テキストコマンドがボトルネックになることはないので、詰める動機も今のところ無い。

### 3.2 リセット手順

1. φM の出力を開始する（PIO を先に動かしておくこと）
2. /IC = L にして `OPM_T_IC_LOW_MS` 保持
3. /IC = H に戻して `OPM_T_IC_WAIT_MS` 待機

φM が停止した状態で /IC を操作しても OPM は初期化されないため、順序が重要。

## 4. YM3012 DAC キャプチャ

OPM が YM3012 (DAC) へ送るシリアル出力を PIO + DMA で取り込み、Core 0 で
signed 16bit little-endian ステレオ PCM へ変換して 2 本目の USB CDC へ流す。
ロジックアナライザを使わずに Pico 2 単体で波形が取れる。

**PIO と DMA は起動時から常時動いている。** `p 1` / `p 0`（[README §3.10](../README.md#310-ppcm-出力)）が
切り替えるのは「リングから読み出して CDC へ流すかどうか」だけで、取り込み自体は止めない。

```
     YM3012
   SO / φ1 / SH1
        │
        ▼
       PIO  ← 1 フレーム 32bit を組む（ビット取り込みだけ）
        │
        ▼
       DMA  ← ハードウェアリングへ書き続ける（割り込みなし）
        │
        ▼
  16KB リングバッファ
        │
        ▼
     Core 0  ← YM3012 形式 → PCM 変換
        │
        ▼
  USB CDC #1（signed 16bit LE ステレオ）
```

### 4.1 フレームの構造

φ1 は φM/2 のビットクロック。1 変換 = φ1 16 クロック、CH1 → CH2 の順に繰り返すので
**1 フレーム = 32 クロック**、出力レートは **φM/64**（φM 4MHz なら 62500Hz）になる。

CH1 の D0 を起点にしたときのフレームの中身は次のとおり。

| φ1 クロック | 内容 |
| --- | --- |
| 0..9   | CH1 仮数 D0..D9 |
| 10..12 | CH1 指数 S0..S2 — 直後に **SH2 が立ち下がる** |
| 13..15 | 無効 ×3 |
| 16..25 | CH2 仮数 D0..D9 |
| 26..28 | CH2 指数 S0..S2 — 直後に **SH1 が立ち下がる** |
| 29..31 | 無効 ×3 |

**CH1 = L / CH2 = R。** SH1 と SH2 はそれぞれ相手チャネルの名前が付いているように見えるが、
SH2 が CH1 の、SH1 が CH2 のサンプルホールドである。したがって、

- **SH1 の立ち下がり**の直後から 32bit 取り込むと `L, R` の順になる
- SH2 の立ち下がりの直後から取り込むと `R, L` の順になってしまう

ので、フレーム同期には **SH1 の立ち下がり**を使う。この対応は
[test/dac_lr/](../test/dac_lr/README.md) で実測により確定している。

### 4.2 PIO によるビット取り込み

[ym3012.pio](../ym3012.pio) の 7 命令。入力は in_base (GP17) から連続 4 本で、
ピンはすべて in_base からのオフセットで参照する（GPIO 番号は現れない）。
**SO / φ1 / SH1 / SH2 を GP17 から連続で割り当てないといけないのはこのため。**

```
.wrap_target
    wait 1 pin 2            ; SH1 が H になるのを待つ
    wait 0 pin 2            ; SH1 立ち下がり = CH2 スロットの終端。次は CH1 (L)
    set  x, 31              ; 1 フレーム = 32bit
bitloop:
    wait 0 pin 1            ; φ1 の L 区間へ入る
    wait 1 pin 1            ; φ1 立ち上がり
    in   pins, 1            ; SO を 1bit 取り込む
    jmp  x--, bitloop
.wrap
```

- **右シフト + autopush（閾値 32）**。YM3012 は LSB first なので、右シフトすると
  先に届いたビットがそのまま LSB 側へ落ちる
- 1 フレーム = 1 エントリ。**L が下位 16bit、R が上位 16bit** に入る。L と R が
  ハードウェア的に不可分なので、途中でずれることがない
- **毎フレーム SH1 で取り直す**ので、一度ずれても次のフレームで復帰する
- TX FIFO は使わないので `PIO_FIFO_JOIN_RX` で RX を深さ 8 にする
- 分周なし。φ1 (2MHz) に対して sys_clk (144MHz) は 1 クロックあたり 72 サイクルある

φM 生成用の PIO ([opm_clock.pio](../opm_clock.pio), 2 命令) と I2S 出力用の PIO
([i2s.pio](../i2s.pio), 8 命令) とは別のステートマシンを使う。3 つとも
`pio_claim_free_sm_and_add_program()` で動的に確保するので、SM も命令メモリのオフセットも
固定していない。合計 17 命令で、RP2350 には PIO ブロックが 3 つ（各 4 SM / 32 命令）
あるので余裕がある。

得られる 32bit ワードのビット配置:

```
bit  0.. 2 : 無効 ×3        bit 16..18 : 無効 ×3
bit  3..12 : L 仮数 D0..D9   bit 19..28 : R 仮数 D0..D9
bit 13..15 : L 指数 S0..S2   bit 29..31 : R 指数 S0..S2
```

### 4.3 PCM への変換

データシート LSI-2130123 (1992-04) より、YM3012 の 1 語は
**無効 3bit + 仮数 10bit + 指数 3bit** で、いずれも LSB first。

- 仮数は 10bit オフセットバイナリ。符号付き値は `m - 512`（範囲 −512..+511）
- 指数は `E = S2*4 + S1*2 + S0`。E が大きいほどステップが粗い（E=1 が最も細かい）
- 線形値は **`pcm = (m - 512) << (E - 1)`**
- **`E = 0` は 3→7 デコーダの禁止コード**で正常な出力には現れない。0 として扱う

値域は **−32768**（m=0, E=7）〜 **+32704**（m=1023, E=7 = 511<<6）。
正側が +32767 に届かないのは仕様どおりで、正規化してはいけない。

実装は [ym3012.h](../ym3012.h) の `ym3012_word_to_pcm()`。

### 4.4 DMA リング

RP2350 のハードウェアリング機能で、PIO の RX FIFO からリングバッファへ書き続ける。

| 項目 | 値 |
| --- | --- |
| 転送サイズ | 32bit（1 フレーム = 1 転送） |
| リングサイズ | 16384 バイト = 4096 フレーム（`channel_config_set_ring(.., true, 14)`） |
| バッファのアライン | 16384 バイト境界（リング機能はアドレス下位ビットだけを回すため） |
| TRANS_COUNT | `dma_encode_endless_transfer_count()`（MODE=ENDLESS） |
| DREQ | `pio_get_dreq(pio, sm, false)` |
| 割り込み | **使わない** |

φM 4MHz では 62500 フレーム/s なので **流入は 250 KB/s**、リングは **65.5ms 分**になる。
これが Core 0 と USB が停滞できる上限。

CPU は `dma_channel_hw_addr(ch)->write_addr` から書き込み位置を読む。リング内の位置は
12bit しか分からないので、差分を積んで 64bit の総フレーム数へ延ばす。呼び出し間隔が
リング一周より短い限りこの差分は一意に決まる。読み出し位置との差が未処理フレーム数で、

| 未処理フレーム数 | 状態 |
| --- | --- |
| 0 | empty |
| 1 .. 4095 | データあり |
| 4096 以上 | **overrun** |

リング末尾をまたぐ読み出しはそこで 2 回に分割する。

**読み出し位置は消費者ごとに持つ。** USB キャプチャと I2S 出力（[§5](#5-i2s-出力)）は
同じリングを別々のペースで読むので、`ym3012_reader_t` を各自が 1 個持ち、
`ym3012_reader_read_pcm()` がそのカーソルだけを進める。書き込み位置
（`s_write_total`）は 1 つで、DMA の位置から更新する。

```c
typedef struct {
    uint64_t read_total;
    bool     count_forbidden;  // 禁止コード E=0 を統計へ数えるか
} ym3012_reader_t;
```

`count_forbidden` を分けているのは、`E0`（[README §3.11](../README.md#311-s統計)）が
「キャプチャしたデータの品質」の指標だから。常時回っている I2S 側で数えると意味が
変わるので、数えるのは USB キャプチャ側のカーソルだけにしている。

引数を取らない `ym3012_unread()` / `ym3012_read_pcm()` などは、USB キャプチャが使う
既定カーソルへのラッパとして残してある。

### 4.5 DMA overrun

未処理がリング全体に達すると、最も古いフレームが DMA に踏まれた可能性がある。
壊れたデータを PCM として送り続けないよう、次のように扱う。

1. キャプチャを中止して PCM 送信を停止する（状態は `ERROR`）
2. コマンド CDC へ `# ERR dma overrun` を出す
3. LED をエラー表示にする（[README §3.9](../README.md#39-led)）
4. 次の `p 1` を受けるまで止まったままにする

**PIO と DMA は止めない。** リングの整合性は次の `p 1` で読み出し位置を取り直して回復する。

`# ` を付けた情報行として出しているのは、裸の `ERR ...` を非同期に出すと
「1 コマンド 1 応答」（[README §3.3](../README.md#33-応答)）を破り、ホスト側のパーサが実行中の
コマンドの応答と取り違えて同期を失うため。状態は `s`（[README §3.11](../README.md#311-s統計)）の
`state` と `OVERRUN` からも読める。

### 4.6 起動時の自己診断

ロジックアナライザを繋がずに PIO のビット順を検証するため、起動時に
**ループバック自己診断**を 1 回だけ実行する。

キャプチャ用 PIO が参照するのは SO / φ1 / SH1 の 3 本だけで、しかも純粋にエッジ駆動で
タイミング制約が無い。そこで DMA を仕掛ける前に、未接続の **GP26-GP28** を CPU から
ビットバンギングして既知の (L, R) を含む合成フレームを流し込み、PIO が push した
32bit ワードと変換後の PCM が期待値と一致するかを確かめる。これで

- SH1 立ち下がりの検出
- φ1 立ち上がりでの SO サンプリング
- LSB first / 右シフト / autopush 32
- 16bit ワードのフィールド配置と L, R の順序

がまとめて検証できる。結果は起動バナーと `t`（[README §3.12](../README.md#312-t自己テスト)）で
確認できる。

**GP26-GP28 は I2S 出力（[§5](#5-i2s-出力)）と同じ 3 本**なので、両立しない。
`YM3012_LOOPBACK_ENABLED` の既定値は `I2S_ENABLED` から決まり、I2S が有効なら診断を
行わず結果は `SKIP (disabled)` になる。DAC を外して診断だけ試すときは
`-DYM3012_LOOPBACK=1` で再コンフィグする。

## 5. I2S 出力

キャプチャした PCM を、そのまま I2S で外部 DAC (PCM5102A) へ流す。
利用者から見た仕様と配線は [README §5](../README.md#5-i2s-出力)。

USB キャプチャと同じリングを [§4.4](#44-dma-リング) の別カーソルで読むので、
`p 1` と同時に動いても互いに干渉しない。**起動時から常時動いていて、停止する手段は無い。**

```
  16KB キャプチャリング（[§4.4](#44-dma-リング)）
        │
        ▼
     Core 0  ← YM3012 形式 → PCM 変換（I2S 用のカーソルで読む）
        │
        ▼
  16KB 出力リング  ← DMA の読み出し位置より 1024 フレーム先を維持する
        │
        ▼
       DMA  ← ハードウェアリングから PIO TX FIFO へ流し続ける（割り込みなし）
        │
        ▼
       PIO  ← BCK / LRCK / DIN を作る
```

### 5.1 PIO プログラム

[i2s.pio](../i2s.pio) の 8 命令。sideset 2bit に BCK / LRCK、OUT 1bit に DIN を割り当てる。
**LRCK は BCK の次の GPIO でなければならない**（sideset は連続したピンにしか出せない）。

```
.side_set 2                         ;  bit1 = LRCK / bit0 = BCK
.wrap_target
bitloop_r:
    out pins, 1        side 0b10    ; BCK L: ビットを出す
    jmp x--, bitloop_r side 0b11    ; BCK H: DAC が拾う。15bit ぶん回る
    out pins, 1        side 0b00    ; 16bit 目。ここで LRCK を先に落とす
    set x, 14          side 0b01
bitloop_l:
    out pins, 1        side 0b00
    jmp x--, bitloop_l side 0b01
    out pins, 1        side 0b10    ; 16bit 目。ここで LRCK を先に上げる
    set x, 14          side 0b11
.wrap
```

1 bit = 2 命令 = 2 サイクル（BCK の L 区間と H 区間）で、1 フレーム = 32bit = **64 サイクル**。
各 16bit ブロックの最終ビットを出すところで LRCK を先に反転させているのが、I2S の
「LRCK 遷移の 1 BCK 後から新しいワードの MSB が始まる」規約にあたる。

**ワードのビット並びが [§4.3](#43-pcm-への変換) の出力形式とそのまま一致する。**

- OSR は MSB first (`shift_right = false`) の 32bit autopull なので、先に出るのは上位 16bit
- 先に出るブロックは LRCK=1 の期間 = Philips I2S の R チャンネル
- キャプチャ側のフレーム形式は「下位 16bit = L (CH1) / 上位 16bit = R (CH2)」

つまり `ym3012_reader_read_pcm()` が `out[2i]=L, out[2i+1]=R` と書いたバッファを
`uint32_t` として DMA で流すだけでよく、**並べ替えや専用の変換関数は要らない**
（リトルエンディアンであることが前提。`_Static_assert` で担保している）。

`pio_sm_init()` はスクラッチレジスタ X を初期化しないので、
`i2s_out_program_init()` の末尾で `pio_sm_exec()` を使って X = 14 を入れている。
SM 停止中の `pio_sm_exec()` は即時実行されるため、呼び出し側が `pio_sm_set_enabled()` を
呼ぶ時点で X は 14 になっている。走り出したあとは各位相の 16bit 目の `set x, 14` が
次の位相ぶんを仕込むので、以後はプログラム内で回る。

これを省くと最初の R ブロックのビット数が 16 からずれ、以後ワード境界と LRCK の位相が
**恒久的に**ずれる（autopull は 32bit ごとに引くのに対し、LRCK は 32bit 周期のままなので
一度ずれると戻らない）。同じ理由で、**SM を再起動する経路を足すときは X を入れ直すこと**。
PC だけプログラム先頭へ戻すと、X が中途半端な値のまま最初の位相のビット数がずれる。

1 フレームが 64 サイクルに厳密に収まっていて、各位相の 16bit 目の BCK H 区間が
その `set x, 14` そのものなので、[§4.2](#42-pio-によるビット取り込み) の取り込み側のように
`.wrap_target` の直後へ初期化用の `set x` を置く余地は無い（1 命令足すとレートが狂う）。

### 5.2 サンプリングレートのロック

φM も I2S も同じ sys_clk を PIO で分周して作っているので、**両者は同一クロック源で
完全にロックする**。リサンプリングもドリフト補正も要らず、固定長のバッファを挟むだけでよい。

1 フレーム = 64 サイクル、fs = φM/64 なので

```
clkdiv_i2s = sys_clk / (64 × fs) = sys_clk / φM
```

φM 側は 2 命令のループなので `clkdiv_opm = sys_clk / (2 × φM)`。したがって

```
clkdiv_i2s = 2 × clkdiv_opm
```

`i2s_init()` は `opm_clock_div_int()` / `opm_clock_div_frac()` から 16.8 固定小数の
分周値を組み立て、**256 倍のまま 2 倍する**。丸めを一度も挟まないので、プリセットが
小数分周になる場合でもレートは厳密に一致する。

既定の 2 プリセットではどちらも整数分周になる。

| プリセット | sys_clk | φM | fs | clkdiv_i2s | BCK (32fs) |
| --- | --- | --- | --- | --- | --- |
| `OPM_CLOCK_MODE_4MHZ` | 144 MHz | 4.000000 MHz | 62500.0 Hz | 36 + 0/256 | 2.000 MHz |
| `OPM_CLOCK_MODE_NTSC` | 157.5 MHz | 3.579545 MHz | 55930.4 Hz | 44 + 0/256 | 1.7898 MHz |

### 5.3 出力リングと先行量の維持

[§4.4](#44-dma-リング) のリングを、読み出し側で回す形に反転させたもの。

| 項目 | 値 |
| --- | --- |
| 転送サイズ | 32bit（1 フレーム = 1 転送） |
| リングサイズ | 16384 バイト = 4096 フレーム（`channel_config_set_ring(.., false, 14)`） |
| バッファのアライン | 16384 バイト境界 |
| TRANS_COUNT | `dma_encode_endless_transfer_count()`（MODE=ENDLESS） |
| DREQ | `pio_get_dreq(pio, sm, true)` |
| 割り込み | **使わない** |
| 先行量 `I2S_TARGET_FRAMES` | 1024 フレーム = 16.4 ms |

リング一周をキャプチャ側と同じ 65.5ms に揃えてあるので、**ポーリング間隔の制約は
増えていない**（[§1.1](#11-メインループ)）。

CPU は `dma_channel_hw_addr(ch)->read_addr` から DMA の読み出し位置を読み、差分を積んで
総フレーム数へ延ばす。自分が書いた総フレーム数との差が「DMA の先をどれだけ走っているか」
= 出力レイテンシで、毎周回これを `I2S_TARGET_FRAMES` まで埋め戻す。

起動時はリングが `.bss` の全ゼロ（= 無音）なので、書き込み位置を先行量ぶん進めた
状態にするだけで先行分の無音が用意できる。

**ソースが空のときに無音を差し込まない。** レートが厳密に一致している以上、ポーリングの
位相差でキャプチャ側が一瞬空になるのは正常な状態。ここで無音を入れると差し込んだ分だけ
キャプチャ側にバックログが溜まり、最後は overrun する。足りない分は次の周回で埋める。

逆に CPU が停滞した場合は、DMA が進んだぶん埋め戻す量が増えるので、次の周回で
バックログをまとめて消費して両者が揃う。**追いつき処理は要らない。**

### 5.4 アンダーラン

ENDLESS のリング DMA は止まらないので、埋め戻しが間に合わないと **DMA は無音ではなく
古いリング内容を再生する**。先行量が 0 以下になったら

1. `UNDERRUN` を 1 増やす（[README §3.11](../README.md#311-s統計)）
2. キャプチャ側のカーソルを書き込み位置へ合わせて、溜まった古いフレームを捨てる
3. 先行分を無音で埋め直して仕切り直す

これを完全に防ぐには DMA を止めるか無音バッファへチェーンする必要があり、
割り込みを使わない構成から外れるので採っていない。

先行量が尽きるのは 16.4ms 停滞したときで、キャプチャ側リングの 65.5ms より
はるかに早い。**アンダーランは必ずキャプチャ側の overrun より先に検出される**ので、
I2S 側のカーソルがキャプチャ側リングを一周遅れることはない。

OPM を繋がずに動かした場合はソースが供給されないため、16.4ms ごとに 1. 〜 3. を
繰り返す。出力は無音のままで、`UNDERRUN` だけが増え続ける。

`r`（/IC リセット）でも必ず 1 回起きる。理由は 2 つあり、どちらも単独で先行量を使い切る。

- `opm_reset()` は `OPM_T_IC_LOW_MS` + `OPM_T_IC_WAIT_MS` = **20ms** をメインループを
  回さずに待つ（[§1.1](#11-メインループ)）。その間 DMA は 1250 フレーム消費するのに
  埋め戻しは走らないので、先行量 16.4ms を超える。
- /IC が L の間は YM3012 が出力を止めるため、**625 フレーム（10ms 相当）がそもそも
  供給されない**。レートが厳密に一致していて余剰が無い（[§5.2](#52-サンプリングレートのロック)）
  以上、この欠損を取り返す方法は無く、先行量から永久に差し引かれる。**先行量を増やしても
  欠損の累積は止まらない。**

欠損量は `s` の `RATE` で確かめられる。`r` を含む 1 秒だけ 61870 frames/s 前後（= 62500 から
620 ほど少ない）に落ち、/IC を使わない `c` では 62500 frames/s のまま減らない。

## 6. USB CDC 2 本構成の実装

コマンド用とキャプチャ用に CDC を 2 本並べる（利用者から見た構成は
[README §3.1](../README.md#31-接続)）。Pico SDK の `stdio_usb` は CDC を 1 本しか
用意しないので、TinyUSB のディスクリプタと初期化はアプリ側で持つ。

- `tinyusb_device` を追加でリンクし、[tusb_config.h](../tusb_config.h) と
  [usb_descriptors.c](../usb_descriptors.c) を自前で用意する
- `tud_task()` はメインループから回す
- **CDC #0 をディスクリプタの先頭に置く。** こうするとコマンド側のデバイス名が
  1 ポート構成のときと変わらず、既存のホスト側スクリプトがそのまま動く。
  CDC #1 は末尾の番号が増えたものになる
- この構成では `PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE` が無効になるため、
  **BOOTSEL へ自動移行する `picotool load -fx` は使えない**
  （[README §6.4](../README.md#64-書き込み代替経路)）

`PICO_STDIO_USB_STDOUT_TIMEOUT_US=10000` を CMake で定義している。既定の 0.5 秒のままだと、
ホストが CDC #0 を読まないときの `printf` が DMA リングの 65.5ms（[§4.4](#44-dma-リング)）を
食い潰す。

## 7. ビルド構成

Pico VS Code 拡張が管理する「DO NOT EDIT」ブロック（`sdkVersion` / `toolchainVersion` /
`picotoolVersion` の設定と `pico-vscode.cmake` の include）は手で書き換えない。
それ以外は次の構成になっている。

- `add_executable` に [§1](#1-ソース構成) の `.c` を並べる
- `pico_set_program_version` はバイナリに埋め込むメタデータ（`picotool info` が読む）。
  `i` と起動バナーが表示する版番号はこれではなく [opm.h](../opm.h) の `OPM_WRITER_VERSION`
- `target_compile_options` に `-Wall -Wextra`（自前のソースだけが対象）
- `pico_generate_pio_header(... opm_clock.pio)` / `(... ym3012.pio)` / `(... i2s.pio)` →
  `build/opm_clock.pio.h` / `build/ym3012.pio.h` / `build/i2s.pio.h` を生成
- `target_link_libraries` は `pico_stdlib` / `hardware_pio` / `hardware_dma` /
  `hardware_clocks` / `tinyusb_device`
- キャッシュ変数 `OPM_CLOCK_MODE` を持ち、指定時のみ同名マクロを
  `target_compile_definitions` で渡す（φM プリセットの切り替え、[§2](#2-φm-の生成)）
- キャッシュ変数 `I2S_ENABLED`（既定 1）は**常に**マクロとして渡す。
  [ym3012.h](../ym3012.h) がこれを見て `YM3012_LOOPBACK_ENABLED` の既定値を決めるため、
  未定義のままにできない（[§4.6](#46-起動時の自己診断)）
- キャッシュ変数 `YM3012_LOOPBACK` は指定時のみ `YM3012_LOOPBACK_ENABLED` として渡し、
  上の自動判定を上書きする
- `PICO_STDIO_USB_STDOUT_TIMEOUT_US=10000` を定義する（[§6](#6-usb-cdc-2-本構成の実装)）
- `pico_enable_stdio_usb 1` / `pico_enable_stdio_uart 0`
- `target_include_directories` にリポジトリ直下を入れる。SDK の tusb_config.h は
  `-isystem` で入るので、これで自前の [tusb_config.h](../tusb_config.h) が優先される

### 7.1 PIO のビルドフロー

`.pio` は `pico_generate_pio_header()` で `build/<名前>.pio.h` に変換され、C 側は
`#include "opm_clock.pio.h"` のように参照する。`.pio` 内の `% c-sdk { ... %}` ブロックは
そのまま生成ヘッダへ展開されるため、`opm_clock_program_init()` のような初期化ヘルパは
ここに書く。

**新しい `.pio` を追加したときは `pico_generate_pio_header()` の行を足して再コンフィグする。**
