# pico-opm-writer

Raspberry Pi Pico 2 (RP2350) を YM2151 (OPM) に直結し、ホスト PC から USB シリアル経由で
OPM のレジスタへ値を書き込むためのファームウェア。

ホストからは USB CDC (仮想 COM ポート) として見え、`screen` や `minicom` などの素の
ターミナルからテキストコマンドを打つだけでレジスタを操作できる。バッチ実行や
DAC 出力のキャプチャには [ホスト側ツール](#8-ホスト側ツール)（`tools/`）を使う。

## 1. ハードウェア構成

### 1.1 接続表

Pico 2 は OPM のバスに対して **書き込み専用** で接続する。データバスは常に Pico 側が駆動する。
これとは別に、YM3012 (DAC) のシリアル出力を Pico 側へ取り込む（[§4](#4-ym3012-dac-キャプチャ)）。

| Pico 2 GPIO | 物理ピン | 接続先 | 方向 | 備考 |
| --- | --- | --- | --- | --- |
| GP0  | 1  | -    | -   | 将来のデバッグ UART TX 用に予約 |
| GP1  | 2  | -    | -   | 将来のデバッグ UART RX 用に予約 |
| GP2  | 4  | D0   | OUT | データバス |
| GP3  | 5  | D1   | OUT | |
| GP4  | 6  | D2   | OUT | |
| GP5  | 7  | D3   | OUT | |
| GP6  | 9  | D4   | OUT | |
| GP7  | 10 | D5   | OUT | |
| GP8  | 11 | D6   | OUT | |
| GP9  | 12 | D7   | OUT | D0-D7 は連続した GPIO であること（マスク書き込みのため） |
| GP10 | 14 | A0   | OUT | L=アドレスラッチ / H=データ書き込み |
| GP11 | 15 | /CS  | OUT | チップセレクト（負論理） |
| GP12 | 16 | /WR  | OUT | 書き込みストローブ（負論理） |
| GP13 | 17 | /RD  | OUT | 読み出しストローブ（負論理）。現在は H 固定 |
| GP14 | 19 | /IC  | OUT | ハードウェアリセット（負論理） |
| GP15 | 20 | φM   | OUT | マスタークロック、PIO で生成 |
| GP16 | 21 | /IRQ | IN  | 割り込み要求（負論理）。プルアップ、レベル参照のみ |
| GP17 | 22 | SO   | IN  | YM3012 シリアルデータ。PIO の in_base |
| GP18 | 24 | φ1   | IN  | YM3012 ビットクロック（φM/2） |
| GP19 | 25 | SH1  | IN  | CH2 のサンプルホールド。フレーム同期に使う |
| GP20 | 26 | SH2  | IN  | CH1 のサンプルホールド。現在は未使用 |
| GP21 | 27 | -    | -   | 将来拡張用に予約 |
| GP22 | 29 | -    | -   | 将来拡張用に予約 |
| GP25 | -  | -    | OUT | 基板上 LED（[§5.9](#59-led)）。`PICO_DEFAULT_LED_PIN` |
| GP26 | 31 | -    | -   | 将来の I2S BCLK 用に予約 |
| GP27 | 32 | -    | -   | 将来の I2S LRCLK / WS 用に予約 |
| GP28 | 34 | -    | -   | 将来の I2S DATA 用に予約 |
| GND  | 3, 8, 13, 18 … | GND | - | OPM と共通グラウンドを取ること |

GP23 / GP24 は Pico 2 の内部用途（`PICO_SMPS_MODE_PIN` / `PICO_VBUS_PIN`）なので使わない。
I2S の MCLK は使わないので、将来の I2S 用に確保するのは GP26-GP28 の 3 本だけ。

SO / φ1 / SH1 / SH2 は **GP17 から連続していること**。PIO がこの 4 本を in_base からの
オフセットで参照する（[§4.2](#42-pio-によるビット取り込み)）。

ピン番号の定義は [opm.h](opm.h) の `OPM_PIN_*` と [ym3012.h](ym3012.h) の `YM3012_PIN_*`
にまとまっている。

### 1.2 GPIO を割り当てない OPM 端子

| OPM 端子 | 処理 | 理由 |
| --- | --- | --- |
| CT1 / CT2 | 未接続で可 | 汎用出力端子。本ファームウェアは使わない |

### 1.3 配線上の注意

- **電源電圧差**: OPM は 5V デバイス、Pico 2 の GPIO は 3.3V。本構成は 3.3V で直接駆動する。
  [test/](test/) の実測データはすべてこの 3.3V 直結の構成で取れているので、**手元の個体では
  動作している**。ただし OPM の入力 H レベル閾値には個体差・ロット差があり、他の個体でも
  同じように動く保証は無い（未確認）。特に **φM が不安定になる可能性がある**ので、
  動作しない場合はバッファ / レベルシフタ（74HCT244 等）を φM とバス側に挿入する。
- **逆流防止**: /RD を H に保っていれば OPM は D0-D7 を駆動しないため、5V が Pico の
  GPIO に流れ込むことはない。ファームウェアは /RD を常に H にしている。
- **OPM 出力のレベル変換は必須**: SO / φ1 / SH1 / SH2 / /IRQ は 5V デバイスである OPM の
  **出力**で、H レベルは 3.3V を大きく超える。RP2350 の GPIO は 5V トレラントではなく
  絶対最大定格 (IOVDD + 0.3V) を超えるので、**レベル変換器または分圧を必ず挟む**こと。
  φ1 は 2MHz で動くので、分圧で済ませる場合は時定数に注意する。
- 電源には十分なデカップリング（OPM の VCC-GND 間に 0.1µF + 10µF 程度）を入れる。

## 2. クロック仕様 (φM)

- 生成方法: PIO ステートマシン 1 基で GPIO をトグルする（1 命令 1 サイクル × 2 で 1 周期）。

```
φM = sys_clk / (2 × clkdiv)
```

φM とシステムクロックは**整数分周（ジッタなし）になる組**でしか意味を持たないため、
両者をペアにしたプリセットとして [opm.h](opm.h) に持たせている。

| プリセット | φM | sys_clk | clkdiv | 備考 |
| --- | --- | --- | --- | --- |
| `OPM_CLOCK_MODE_4MHZ`（**既定**） | 4.000000 MHz | 144 MHz（= 4MHz × 36） | 18 | YM2151 の定格上限。RP2350 定格 150MHz 以内 |
| `OPM_CLOCK_MODE_NTSC` | 3.579545 MHz | 157.5 MHz（= × 44） | 22 | NTSC カラーサブキャリア = 315/88 MHz。定格比 約 +5% の OC |

切り替えは次のどちらかで行う。

- `opm.h` の `#define OPM_CLOCK_MODE OPM_CLOCK_MODE_4MHZ` の行を書き換える
- ヘッダを触らずに再コンフィグする:
  ```bash
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 -DOPM_CLOCK_MODE=1
  ```
  `-DOPM_CLOCK_MODE=` （空）に戻すと `opm.h` の既定値に従う。

`OPM_CLOCK_HZ` / `OPM_SYS_CLOCK_KHZ` をプリセット外の値へ直接書き換えることもできるが、
その場合は **必ず両方を整数分周になる組で**指定すること（片方だけの変更は小数分周になる）。

### 2.1 システムクロックを φM に合わせる理由

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

[opm.h](opm.h) の定数で調整する。クロック依存の待ち時間は `clock_get_hz(clk_sys)` から
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
仮置きである。** この値で [test/](test/) の全キャプチャ（数千条件）が取れているので
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

**PIO と DMA は起動時から常時動いている。** `p 1` / `p 0`（[§5.10](#510-ppcm-出力)）が
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
[test/dac_lr/](test/dac_lr/README.md) で実測により確定している。

### 4.2 PIO によるビット取り込み

[ym3012.pio](ym3012.pio) の 7 命令。入力は in_base (GP17) から連続 4 本で、
ピンはすべて in_base からのオフセットで参照する（GPIO 番号は現れない）。

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

φM 生成用の PIO ([opm_clock.pio](opm_clock.pio), 2 命令) とは別のステートマシンを使う。
どちらも `pio_claim_free_sm_and_add_program()` で動的に確保するので、命令メモリの
使用量は 32 命令中 9 命令。

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

実装は [ym3012.h](ym3012.h) の `ym3012_word_to_pcm()`。ホスト側デコーダ
[tools/opm-dac2wav.py](tools/opm-dac2wav.py) の `PCM_LUT` と同じ式なので、
どちらの経路で取っても同じ値になる。

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

### 4.5 DMA overrun

未処理がリング全体に達すると、最も古いフレームが DMA に踏まれた可能性がある。
壊れたデータを PCM として送り続けないよう、次のように扱う。

1. キャプチャを中止して PCM 送信を停止する（状態は `ERROR`）
2. コマンド CDC へ `# ERR dma overrun` を出す
3. LED をエラー表示にする（[§5.9](#59-led)）
4. 次の `p 1` を受けるまで止まったままにする

**PIO と DMA は止めない。** リングの整合性は次の `p 1` で読み出し位置を取り直して回復する。

`# ` を付けた情報行として出しているのは、裸の `ERR ...` を非同期に出すと
「1 コマンド 1 応答」（[§5.3](#53-応答)）を破り、ホスト側のパーサが実行中のコマンドの
応答と取り違えて同期を失うため。状態は `s`（[§5.11](#511-s統計)）の `state` と
`OVERRUN` からも読める。

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

がまとめて検証できる。結果は起動バナーと `t`（[§5.12](#512-t自己テスト)）で確認できる。

GP26-GP28 に何かを接続する場合は、`YM3012_LOOPBACK_ENABLED` を 0 にして診断を止める。

## 5. ホストインタフェース

### 5.1 接続

- USB CDC (仮想 COM ポート) **2 ポート**。

  | | 用途 | macOS でのデバイス名の例 |
  | --- | --- | --- |
  | CDC #0 | コマンド。Pico SDK の `stdio_usb` がそのまま使う | `/dev/cu.usbmodem112101` |
  | CDC #1 | PCM データ出力（[§4](#4-ym3012-dac-キャプチャ)） | `/dev/cu.usbmodem112103` |

  CDC #0 をディスクリプタの先頭に置いているので、コマンド側のデバイス名は 1 ポート構成の
  ときと変わらない。CDC #1 は末尾の番号が増えたものになる。
- CDC を 2 本にするため TinyUSB のディスクリプタと初期化はアプリ側で持つ
  （`tinyusb_device` を追加でリンクし、[tusb_config.h](tusb_config.h) と
  [usb_descriptors.c](usb_descriptors.c) を自前で用意する）。`tud_task()` はメインループから回す。
  この構成では `PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE` が無効になるため、
  **`picotool load -fx`（BOOTSEL 不要の書き込み）は使えない**（[§7.3](#73-書き込み代替経路)）。
- コマンド側の入出力はすべて標準入出力 API 経由:
  - 出力: `printf()` / `puts()`
  - 入力: `getchar_timeout_us()` でノンブロッキングに 1 文字ずつ受け取り、行バッファに溜める
  - 接続検出: `stdio_usb_connected()`
- ボーレート・パリティ等の設定は無視される（USB CDC のため何を指定しても動作する）。
- macOS 例: `screen /dev/cu.usbmodem112101 115200`。
  **`/dev/tty.*` ではなく `/dev/cu.*` を使う**（`tty.*` は DCD 待ちで open がブロックする）。
  デバイス名の引き直し方は [§7.4](#74-シリアル-stdio-の読み取り) を参照。
- 接続を検出した時点で起動バナーを出力する（接続前の出力は捨てられるため）。

### 5.2 行フォーマット

- コマンドは **1 行 1 コマンド**。行末は `LF` / `CR` / `CRLF` のいずれでも受け付ける。
- 1 行の最大長は 255 文字（`LINE_MAX_LEN`、[pico-opm-writer.c](pico-opm-writer.c) で定義）。
  超えた場合は `ERR too long` を返し、その行を破棄する。
- コマンド文字・16 進数の大文字小文字は区別しない。
- トークン区切りは空白またはタブ（連続可）。
- 空行、および **行頭が `#` の行**はコメントとして無視し、**応答も返さない**。

**行末コメント（`w 20 c7 # ...`）には対応していない。** `#` は引数のトークンとして読まれ、
`ERR bad argument` または `ERR wrong arity` になる。行末コメント・プレースホルダ置換・
キャプチャ制御が要るシーケンスは [tools/opm-writer.py](docs/opm-writer.md) 経由で流し込む。

### 5.3 応答

各コマンドは必ず 1 行の応答で終わる。

| 応答 | 意味 |
| --- | --- |
| `OK` | 正常終了 |
| `ERR <理由>` | エラー |

情報を返すコマンドは、`#` で始まる情報行を 0 行以上出力したあと最後に `OK` を返す。
ホスト側は「`#` 始まりは情報、それ以外の 1 行が終端」と扱えばよい。

エラー理由の一覧:

| 応答 | 発生条件 |
| --- | --- |
| `ERR unknown command` | 未知のコマンド文字。コマンドトークンが 2 文字以上の場合も含む（コマンドはちょうど 1 文字） |
| `ERR bad argument` | 引数が 16 進数 / 10 進数として解釈できない |
| `ERR wrong arity` | 引数の個数が合わない |
| `ERR out of range` | 引数が許容範囲外 |
| `ERR too long` | 行が長すぎる |

### 5.4 コマンド一覧

| コマンド | 書式 | 説明 |
| --- | --- | --- |
| `w` | `w <addr> <data> [<addr> <data> ...]` | レジスタ書き込み。引数は 16 進。addr/data のペアを 1 行に複数並べて連続書き込みできる。**途中でエラーになった場合、そこまでの書き込みは実行済み**のまま `ERR` を返す |
| `r` | `r` | /IC によるハードウェアリセット（[§3.2](#32-リセット手順)） |
| `c` | `c` | ソフトウェアによる全レジスタクリア（[§5.5](#55-cクリアが書き込む内容)） |
| `d` | `d <ms>` | 指定ミリ秒待機。10 進、`0`-`60000`（`DELAY_MAX_MS`）。待っている間も PCM の送出は続く |
| `p` | `p 1` / `p 0` | PCM 出力の開始 / 停止（[§5.10](#510-ppcm-出力)） |
| `s` | `s` / `s 0` | 統計の表示 / リセット（[§5.11](#511-s統計)） |
| `t` | `t` | 自己テスト（[§5.12](#512-t自己テスト)） |
| `i` | `i` | 情報表示（[§5.6](#56-i情報表示の出力例)） |
| `h` | `h` / `?` | コマンド一覧を表示 |

16 進引数の桁数は自由で、値が `0xff` 以下なら受理する（`w f 1` も `w 0020 00c7` も通る）。
`0xff` を超えると `ERR out of range`。

### 5.5 `c`（クリア）が書き込む内容

/IC リセットに近い状態をソフトウェアで作る。実行順:

1. `0x08` に `0x00`-`0x07` を書き、全 8 チャンネルを KEY OFF
2. `0x60`-`0x7F`（全 32 スロットの TL）に `0x7F`（最小音量）
3. `0x0F` に `0x00`（ノイズ off）
4. `0x14` に `0x00`（タイマ / IRQ 停止）
5. `0x01` に `0x00`（LFO リセット解除）
6. `0x18` に `0x00`（LFO 周波数）
7. `0x19` に **`0x80` → `0x00` の順で 2 回**書き込む。`0x19` は bit7 で書き込み先が切り替わる
   レジスタで、`0x80` が PMD = 0、`0x00` が AMD = 0 の設定になる（PMD 側を消し忘れないため
   両方を書く）
8. `0x1B` に `0x00`（LFO 波形 / CT1・CT2）
9. `0x20`-`0x27` に `0x00`（RL / FB / CONNECT）

### 5.6 `i`（情報表示）の出力例

```
# pico-opm-writer 0.2.0
# sys_clk : 144000000 Hz
# phiM    : 4000000 Hz (clkdiv 18 + 0/256)
# pins    : D0-D7=GP2-GP9 A0=GP10 /CS=GP11 /WR=GP12 /RD=GP13 /IC=GP14
# pins    : phiM=GP15 /IRQ=GP16
# timing  : t_wr=1us t_addr=5us t_data=25us
# ym3012  : SO=GP17 phi1=GP18 SH1=GP19 SH2=GP20
# capture : ring 16384 bytes (4096 frames) rate 62500 Hz
# selftest: pio PASS
OK
```

### 5.7 起動バナー

USB CDC の接続を検出した時点で `i` と同じ内容を出力する（末尾の `OK` を含む）。

### 5.8 使用例

```
> i
# pico-opm-writer 0.1.0
...
OK
> r
OK
> w 20 c7 28 4a 30 00
OK
> d 500
OK
> w 08 00
OK
```

### 5.9 LED

基板上 LED (`PICO_DEFAULT_LED_PIN`、pico2 では GP25) で状態を示す。表示は 100ms
スロットのパターン駆動で、`sleep_ms()` などで処理を止めることはない。

| 状態 | パターン | 意味 |
| --- | --- | --- |
| 待機 | 常時点灯 | 電源が入っていて、キャプチャしておらず、エラーでもない |
| キャプチャ中 | 500ms ON / 500ms OFF | `p 1` から `p 0` の完了まで |
| コマンド受信 | 100ms ON → 100ms OFF → 100ms ON | 1 回だけ差し込む。終わったら元のパターンの先頭へ戻る |
| DMA overrun | 100ms ON/OFF ×3 → 1 秒 OFF | [§4.5](#45-dma-overrun)。エラー表示はこれだけ |

キャプチャ中の 500ms 点滅とコマンド受信の 100ms 二重点滅は速さで区別できる。

**LED をエラー表示にするのは DMA overrun のときだけ。** `p` の状態エラー・CDC #1 の切断・
USB 未接続・一時的な USB 送信不可は、いずれもエラー表示にしない。

### 5.10 `p`（PCM 出力）

CDC #1 への PCM 送信を制御する。**PIO と DMA はこのコマンドでは開始も停止もしない**
（[§4](#4-ym3012-dac-キャプチャ)）。

| 書式 | 動作 |
| --- | --- |
| `p 1` | このコマンドを処理した時点以降に DMA が書いたデータから送信を始める。それより前のデータは送らない |
| `p 0` | このコマンドを処理した時点までに DMA が取り込んだデータを**最後の 1 フレームまで送り切ってから**停止する |

`p 0` はドレインが終わってから `OK` を返す。待っている間も PCM の送出と USB の処理は
回り続けるので、DMA が止まることはない。2 秒で終わらなければ打ち切って
`ERR drain timeout` を返す（ホストが CDC #1 を読んでいない場合）。

エラーになる条件:

| 状況 | 応答 |
| --- | --- |
| 待機中に `p 0` | `ERR wrong state` |
| キャプチャ中に `p 1` | `ERR wrong state` |
| CDC #1 が開かれていない状態で `p 1` | `ERR not connected` |

いずれもコマンドの状態エラーであって、ファームウェアのエラー状態にはしない
（LED も変えない）。CDC #1 が開かれていないまま送信を始めるとリング 65.5ms 分で
必ず overrun するため、`p 1` の時点で弾いている。

キャプチャ中に CDC #1 が閉じられたら**即座に送信を停止**して待機へ戻る。これはエラーでは
ないので LED も変わらない。再接続しても自動では再開せず、次の `p 1` を待つ。

### 5.11 `s`（統計）

`s` で実行時の統計を表示し、`s 0` でリセットする。単位はすべてバイト。

```
# state   : CAPTURING
# CPU     : 58% (max 58%)
# RING    : 4/16384 bytes  MAX 108/16384  FREE 16380
# USB_TX  : 0/4096 bytes  MAX 100/4096
# OVERRUN : 0   E0 : 0   RXSTALL : 0
# RATE    : 62500 frames/s (expect 62500)
# FRAMES  : 56263161
# PIOTEST : PASS
# IRQ     : H
OK
```

| 項目 | 内容 |
| --- | --- |
| `state` | `IDLE` / `CAPTURING` / `DRAINING` / `ERROR` |
| `CPU` | 直近 1 秒の Core 0 使用率と、リセット以降の最大値。USB の空き待ちで何も送れなかった周回は idle として数える |
| `RING` | DMA リングの未処理量と high-water、空き |
| `USB_TX` | CDC #1 の送信バッファ滞留量と high-water。USB エンドポイントの状態ではなく、ファーム内の TX FIFO の滞留量 |
| `OVERRUN` | DMA overrun の発生回数 |
| `E0` | YM3012 の禁止コード `E=0` を見た数。**PIO のビット位相が正しければ 0 のまま** |
| `RXSTALL` | PIO の RX FIFO があふれた回数。あふれると L/R の並びが崩れる |
| `RATE` | 直近 1 秒で数えた実測フレームレートと期待値 φM/64 |
| `FRAMES` | 取り込んだ総フレーム数 |
| `PIOTEST` | 起動時の PIO ループバック自己診断の結果（[§4.6](#46-起動時の自己診断)） |
| `IRQ` | OPM の /IRQ の現在のレベル |

`E0` / `RXSTALL` / `RATE` は、ロジックアナライザを繋がずにキャプチャ経路の健全性を
確かめるための指標。

### 5.12 `t`（自己テスト）

外部機器を使わずに実行できる自己テストをまとめて走らせる。

```
> t
# pcm     : PASS
# pio     : PASS
OK
```

| 項目 | 内容 |
| --- | --- |
| `pcm` | PCM 変換の既知ベクタ検証。ゼロ / ±1 / 仮数境界 / 指数全域 / 禁止コード / 無効 3bit のマスク / 値域の両端。加えて全 `E`・全仮数でステップが `1 << (E-1)` になることを総当たりで確認する |
| `pio` | 起動時に実施した PIO ループバック自己診断の結果（[§4.6](#46-起動時の自己診断)） |

どちらかが失敗したら `ERR self test failed` を返す。

## 6. ファームウェア構成

| ファイル | 役割 |
| --- | --- |
| [pico-opm-writer.c](pico-opm-writer.c) | `main()`、システムクロック設定、初期化、メインループ、行入力とコマンドパーサ、応答出力 |
| [opm.h](opm.h) / [opm.c](opm.c) | OPM のピン割り当て・タイミング定数・φM プリセット（`OPM_CLOCK_MODE`）、バス制御 |
| [opm_clock.pio](opm_clock.pio) | φM 生成用 PIO プログラムと `opm_clock_program_init()` |
| [ym3012.h](ym3012.h) / [ym3012.c](ym3012.c) | YM3012 の PIO + DMA リング初期化、リング位置の管理、PCM 変換、自己診断 |
| [ym3012.pio](ym3012.pio) | キャプチャ用 PIO プログラムと `ym3012_capture_program_init()` |
| [capture.h](capture.h) / [capture.c](capture.c) | キャプチャの状態機械と、リング → PCM → CDC #1 の送出 |
| [usb_pcm.h](usb_pcm.h) / [usb_pcm.c](usb_pcm.c) | CDC #1 の接続判定・書き込み・滞留量 |
| [led.h](led.h) / [led.c](led.c) | 非ブロッキングな LED パターン表示 |
| [stats.h](stats.h) / [stats.c](stats.c) | CPU 使用率・high-water・カウンタ |
| [tusb_config.h](tusb_config.h) / [usb_descriptors.c](usb_descriptors.c) | USB CDC 2 本構成の TinyUSB 設定とディスクリプタ |

メインループは `tud_task()` → キャプチャの送出 → LED → 統計 → コマンド 1 文字読み、を
回すだけ。`d` の待機と `p 0` のドレイン待ちからも同じ処理を呼ぶので、コマンドの
待ち時間の中でも PCM の送出と USB の処理は止まらない。

主要 API:

```c
void opm_init(void);                        // GPIO と PIO(φM) を初期化し /IC リセットを実行
void opm_write(uint8_t addr, uint8_t data); // 1 レジスタ書き込み（アドレス→データの 2 サイクル）
void opm_reset(void);                       // /IC によるハードウェアリセット
void opm_clear(void);                       // ソフトウェアによる全レジスタクリア

uint32_t opm_clock_hz_actual(void);         // 実際に生成されている φM 周波数
uint32_t opm_clock_div_int(void);           // PIO 分周比の整数部
uint32_t opm_clock_div_frac(void);          // PIO 分周比の小数部（/256）
```

クロック照会の 3 関数は [§5.6](#56-i情報表示の出力例) の情報表示に使う。

### 6.1 CMakeLists.txt の構成

Pico VS Code 拡張が管理する「DO NOT EDIT」ブロック（`sdkVersion` / `toolchainVersion` /
`picotoolVersion` の設定と `pico-vscode.cmake` の include）は手で書き換えない。
それ以外は次の構成になっている。

- `add_executable` に [§6](#6-ファームウェア構成) の `.c` を並べる
- `pico_set_program_version` はバイナリに埋め込むメタデータ（`picotool info` が読む）。
  `i` と起動バナーが表示する版番号はこれではなく [opm.h](opm.h) の `OPM_WRITER_VERSION`
- `target_compile_options` に `-Wall -Wextra`（自前のソースだけが対象）
- `pico_generate_pio_header(... opm_clock.pio)` と `(... ym3012.pio)` →
  `build/opm_clock.pio.h` / `build/ym3012.pio.h` を生成
- `target_link_libraries` は `pico_stdlib` / `hardware_pio` / `hardware_dma` /
  `hardware_clocks` / `tinyusb_device`
- キャッシュ変数 `OPM_CLOCK_MODE` を持ち、指定時のみ同名マクロを
  `target_compile_definitions` で渡す（φM プリセットの切り替え、[§2](#2-クロック仕様-φm)）
- `PICO_STDIO_USB_STDOUT_TIMEOUT_US=10000` を定義する。既定の 0.5 秒のままだと、
  ホストが CDC #0 を読まないときの `printf` が DMA リングの 65.5ms を食い潰す
- `pico_enable_stdio_usb 1` / `pico_enable_stdio_uart 0`
- `target_include_directories` にリポジトリ直下を入れる。SDK の tusb_config.h は
  `-isystem` で入るので、これで自前の [tusb_config.h](tusb_config.h) が優先される

**新しい `.pio` を追加したときは `pico_generate_pio_header()` の行を足して再コンフィグする。**

## 7. ビルドと書き込み

ツールチェーンは `~/.pico-sdk/` 配下にバージョン固定でインストールされている。
**システムの cmake / ninja / arm-none-eabi-gcc は使わない。**

| 用途 | パス |
| --- | --- |
| cmake | `~/.pico-sdk/cmake/v4.3.4/bin/cmake` |
| ninja | `~/.pico-sdk/ninja/v1.13.2/ninja` |
| toolchain | `~/.pico-sdk/toolchain/15_2_Rel1/bin` |
| picotool | `~/.pico-sdk/picotool/2.3.0/picotool/picotool` |
| openocd | `~/.pico-sdk/openocd/0.12.0+dev` |

VS Code のターミナルでは `.vscode/settings.json` がこれらを PATH と環境変数に設定する。
CLI から直接叩く場合は自分で export する:

```bash
export PICO_SDK_PATH=~/.pico-sdk/sdk/2.3.0
export PICO_TOOLCHAIN_PATH=~/.pico-sdk/toolchain/15_2_Rel1
export PATH=~/.pico-sdk/toolchain/15_2_Rel1/bin:~/.pico-sdk/picotool/2.3.0/picotool:~/.pico-sdk/cmake/v4.3.4/bin:~/.pico-sdk/ninja/v1.13.2:$PATH
```

### 7.1 ビルド

```bash
# 増分ビルド（通常はこれだけで足りる）
ninja -C build

# 再コンフィグ（CMakeLists.txt や .pio を変更したとき）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2
```

`build/` は Release / `rp2350-arm-s` で構成済み。`.vscode/tasks.json` の "Compile Project"
タスクが増分ビルドと等価。

成果物は `build/pico-opm-writer.{uf2,elf,bin,hex,dis}` と `build/compile_commands.json`。

### 7.2 書き込み（SWD / PicoProbe が既定）

PicoProbe (Debugprobe on Pico、CMSIS-DAP、VID:PID `2e8a:000c`) をターゲット pico2 の SWD に
接続している場合、**CLI からはこれで焼くのが既定**。BOOTSEL 操作もターゲットの USB 状態も
要らない。

```bash
~/.pico-sdk/openocd/0.12.0+dev/openocd \
  -s ~/.pico-sdk/openocd/0.12.0+dev/scripts \
  -f interface/cmsis-dap.cfg \
  -f target/rp2350.cfg \
  -c "adapter speed 5000; program build/pico-opm-writer.elf verify reset exit"
```

`.vscode/tasks.json` の "Flash" タスクと等価。成功時は `** Programming Finished **` /
`** Verified OK **` / `** Resetting Target **` が出る。`openocd` は PATH に無いので
フルパスで叩く。DAP に繋がらなくなったら `target/rp2350-rescue.cfg` を使う
"Rescue Reset" タスク相当を先に実行する。

書き込みには `.elf` を使う（`.uf2` ではない）。

### 7.3 書き込み（代替経路）

ターゲットの USB 側が生きている場合のみ:

```bash
picotool load build/pico-opm-writer.uf2 -fx   # BOOTSEL 不要、書き込み後に実行まで行う
```

または `build/pico-opm-writer.uf2` を BOOTSEL 起動時の RPI-RP2 ドライブへコピーする。

GUI でのステップ実行デバッグは VS Code の "Pico Debug (Cortex-Debug)" 構成を使う。

### 7.4 シリアル (stdio) の読み取り

| デバイス | 中身 |
| --- | --- |
| `/dev/cu.usbmodem112101` | **ターゲット pico2 自身の USB CDC** = `printf` の出力先 |
| `/dev/cu.usbmodem112202` | PicoProbe 側の CDC-UART ブリッジ（現状の設定では未使用） |

tty 名は USB のポート位置に依存する。変わったら `ioreg -r -c IOSerialBSDClient -l -w 0` の
`locationID` と `ioreg -p IOUSB -l -w 0` の `USB Product Name` を突き合わせて引き直す
（ターゲットは Product Name `Pico`、プローブは `Debugprobe on Pico _CMSIS_DAP_`）。

注意点:

- **`/dev/tty.*` ではなく `/dev/cu.*` を使う。** `tty.*` は DCD 待ちで open がブロックする。
- **SWD の `reset` でターゲットの USB CDC が切断・再列挙される。** 書き込み直後は
  デバイスノードが数秒消えるので、存在を待ってから開く。
- `stdio_usb` はホストが開く前の出力を捨てるため、起動直後の行は取り逃す。

## 8. ホスト側ツール

`tools/` にホスト PC 側のスクリプトを置いている。いずれも Python 3 の標準ライブラリだけで
動く（`.zst` を扱う機能のみ Python 3.14 以上が必要）。

| スクリプト | 役割 | ドキュメント |
| --- | --- | --- |
| `tools/opm-writer.py` | シーケンスファイルを USB CDC 経由でファームへ流し込む。行末コメント / `@KEY@` 置換 / `!capture` によるキャプチャに対応。`--capture-mode` でロジアナ経由と CDC #1 の PCM 経由を切り替える | [docs/opm-writer.md](docs/opm-writer.md) |
| `tools/opm-dac2wav.py` | OPM の DAC 出力 (SO/SH1/SH2) の生ロジックキャプチャを 16bit ステレオ WAV へデコードする | [docs/opm-dac2wav.md](docs/opm-dac2wav.md) |
| `tools/opm-dac-testgen.py` | `opm-dac2wav.py` の回帰テスト（実機不要） | [docs/opm-dac-testgen.md](docs/opm-dac-testgen.md) |
| `tools/opm-lfo-period.py` | 実機キャプチャから LFO の更新周期をサンプル数で測る（`--mode am` / `--mode pm`）。結果は 1 ファイル 1 行の TSV | [docs/opm-lfo-period.md](docs/opm-lfo-period.md) |
| `tools/opm-lfo-period-testgen.py` | `opm-lfo-period.py` の回帰テスト（実機不要） | [docs/opm-lfo-period-testgen.md](docs/opm-lfo-period-testgen.md) |

**ロジックアナライザが無くても `test/` の掃引はそのまま実行できる。**
`tools/opm-writer.py --capture-mode pcm` を使うと、`!capture` の取得経路が
ファームの CDC #1 から読む PCM に切り替わる。出力の形式（`.wav` / `.wav.zst`）と
サンプリングレート（φM/64）は変わらないので、シーケンスファイルも解析ツールも
そのまま使える。掃引スクリプトへは `--capture-mode` をそのまま渡せる。

```bash
./test/dac_lr/capture_all.py --capture-mode pcm --analyze
```

`test/` 以下は、これらを使った実機調査の一次データ生成環境。掃引スクリプトと測定条件は
それぞれの README にまとめてある。

| ディレクトリ | 調べていること |
| --- | --- |
| [test/lfo_noise/](test/lfo_noise/README.md) | LFO ノイズ波形（`LFRQ` / `NFRQ` の掃引） |
| [test/noise_period/](test/noise_period/README.md) | ノイズ発生器そのもの（NE でノイズを直接 DAC へ出す） |
| [test/dac_lr/](test/dac_lr/README.md) | DAC の 2 スロット (CH1/CH2) の関係 |

## 9. 動作確認手順

新しい環境で組んだときに一通り確かめる**手順**。1. は毎回実行して回帰検査に使うもの、
2.〜5. は配線とファームを立ち上げるときの確認項目。

1. **ホスト側の自動テスト**: 実機もロジアナも要らない。いずれも全ケース `PASS` すること。

   | コマンド | 対象 | 所要 |
   | --- | --- | --- |
   | `./tools/opm-dac-testgen.py` | DAC デコーダ (`opm-dac2wav.py`) | 1 秒 |
   | `./tools/opm-lfo-period-testgen.py` | LFO 周期解析 (`opm-lfo-period.py`) | 25 秒 |
   | `./test/dac_lr/lr_relation.py --self-test` | L/R 判定器 | 1 秒 |
   | `./test/lfo_noise/analyze_lfo.py --self-test` | 段ごとの LFO 値と値列の突き合わせ | 10 秒 |
   | `./test/noise_period/analyze_noise.py --self-test` | ノイズ発生器の周期推定 | 10 秒 |

2. **単体（OPM 未接続）**: 書き込み後にシリアル接続し、起動バナーが出ることと `i` の表示が
   期待どおりであることを確認する。CDC が 2 本列挙されることも見ておく。
   続けて `t` を実行し、PCM 変換と PIO ループバックが両方 `PASS` になることを確認する
   （どちらも外部機器が要らない）。
3. **φM の確認**: GP15 をオシロ / 周波数カウンタで測定し、既定プリセットなら
   4.000000MHz（`OPM_CLOCK_MODE_NTSC` なら 3.579545MHz）± 数十 ppm であることと、
   デューティが 50% であることを確認する。
4. **バス波形の確認**: `w 20 c7` を実行し、A0 / /WR / D0-D7 が [§3](#3-opm-バス書き込みシーケンス) の
   シーケンスどおりに動いていることをロジックアナライザで確認する。
5. **発音テスト**: 以下を流し込んで音が出ることを確認する。CONNECT=7 は 4 オペレータすべてが
   キャリアになる接続なので、4 スロット分の設定を同じ値にしておけばスロットの並び順に
   依存せず必ず発音する。

```
# リセットして初期化
r
w 0f 00
w 18 00
w 19 80
w 19 00
w 1b 00
# ch0: RL=both, FB=0, CONNECT=7（全オペレータが出力）
w 20 c7
# 音程（KC / KF）
w 28 4a
w 30 00
# 4 スロット分の DT1/MUL, TL, KS/AR, AMS-EN/D1R, DT2/D2R, D1L/RR
w 40 01 48 01 50 01 58 01
w 60 20 68 20 70 20 78 20
w 80 1f 88 1f 90 1f 98 1f
w a0 00 a8 00 b0 00 b8 00
w c0 00 c8 00 d0 00 d8 00
w e0 0f e8 0f f0 0f f8 0f
# ch0 の全スロットを KEY ON
w 08 78
d 1000
# KEY OFF
w 08 00
```

6. **キャプチャ経路の確認**: 5. の音を鳴らしたまま `p 1` → `s` → `p 0` を実行し、
   `s` が次の状態になっていることを確認する。

   | 項目 | 期待値 | 外れたときに疑うもの |
   | --- | --- | --- |
   | `RATE` | φM/64（既定なら 62500 frames/s） | φ1 / SH1 の配線、PIO のフレーム同期 |
   | `E0` | 0 のまま | ビット位相のずれ |
   | `RXSTALL` | 0 のまま | DMA が FIFO を吸い切れていない |
   | `OVERRUN` | 0 のまま | ホストが CDC #1 を読めていない |

7. **L/R の確認**: `w 20 47`（L のみ）で鳴らして取ったキャプチャの R が全サンプル
   厳密に 0、`w 20 87`（R のみ）で L が全サンプル厳密に 0 になることを確認する。
   逆になっていたら L/R が入れ替わっている。`test/dac_lr/capture_all.py --capture-mode pcm
   --analyze` がこれを含めた判定を一通り行う。

ファームウェア自体のホスト上での自動テストは無い。検証は
**ビルド → 書き込み → シリアル出力の確認** の 3 ステップで行い、外部機器を使わない
範囲の検証は `t` と `s` にまとめてある。

## 10. 将来の拡張（本仕様の範囲外）

- バイナリストリーミングモード（`w` の 1 行あたりのオーバーヘッド削減）
- 時刻付きレジスタ列の一括転送とファーム側タイマによる再生（VGM 再生の下地）
- `/RD` を使ったステータス（BUSY）ポーリングによる待ち時間の最適化。配線と
  データバスの向き切り替え（`opm_bus_set_dir()`）は用意してあるが、読み出し自体は未実装
- `/IRQ` の割り込み処理。現在はレベルを参照できるだけ（`s` の `IRQ`）
- I2S 出力（GP26-GP28 を予約済み。MCLK は使わない 3 線）
- バス書き込みの PIO 化と FIFO による非同期キューイング
- 2 個目の OPM / 他の Yamaha 音源チップ (OPN 系) への対応

## 11. ライセンス

MIT License。詳細は [LICENSE](LICENSE) を参照。

ただし `pico_sdk_import.cmake` は Raspberry Pi (Trading) Ltd. 由来のファイルで、
BSD-3-Clause が適用される（ファイル先頭の表示のとおり）。
