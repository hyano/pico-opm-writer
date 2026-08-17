# pico-opm-writer

Raspberry Pi Pico 2 (RP2350) を YM2151 (OPM) に直結し、ホスト PC から USB シリアル経由で
OPM のレジスタへ値を書き込むためのファームウェア。

ホストからは USB CDC (仮想 COM ポート) として見え、`screen` や `minicom` などの素の
ターミナルからテキストコマンドを打つだけでレジスタを操作できる。バッチ実行や
DAC 出力のキャプチャには [ホスト側ツール](#7-ホスト側ツール)（`tools/`）を使う。

**USB CDC を 2 本（コマンド用 / PCM キャプチャ用）持つ**のが特徴で、ターミナルから
レジスタを叩きながら、同時に YM3012 (DAC) の出力をもう 1 本のポートから PCM として
取り込める（[§4](#4-pcm-出力)）。ロジックアナライザや外部の録音機材は要らない。

ファームウェアの内部設計と実装は [docs/pico-opm-writer.md](docs/pico-opm-writer.md) にある。

## 1. ハードウェア構成

### 1.1 接続表

Pico 2 は OPM のバスに対して **書き込み専用** で接続する。データバスは常に Pico 側が駆動する。
これとは別に、YM3012 (DAC) のシリアル出力を Pico 側へ取り込む（[§4](#4-pcm-出力)）。

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
| GP25 | -  | -    | OUT | 基板上 LED（[§3.9](#39-led)）。`PICO_DEFAULT_LED_PIN` |
| GP26 | 31 | BCK  | OUT | I2S ビットクロック（[§5](#5-i2s-出力)） |
| GP27 | 32 | LRCK | OUT | I2S ワードセレクト。BCK の次の GPIO であること |
| GP28 | 34 | DIN  | OUT | I2S データ |
| GND  | 3, 8, 13, 18 … | GND | - | OPM と共通グラウンドを取ること |

GP23 / GP24 は Pico 2 の内部用途（`PICO_SMPS_MODE_PIN` / `PICO_VBUS_PIN`）なので使わない。
I2S の MCLK は使わないので、DAC へ出すのは GP26-GP28 の 3 本だけ。

SO / φ1 / SH1 / SH2 は **GP17 から連続していること**。キャプチャ用の PIO がこの 4 本を
in_base からのオフセットで参照する（[docs §4.2](docs/pico-opm-writer.md#42-pio-によるビット取り込み)）。

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

## 2. クロック設定 (φM)

φM は PIO で生成する。φM とシステムクロックは**整数分周（ジッタなし）になる組**でしか
意味を持たないため、両者をペアにしたプリセットとして [opm.h](opm.h) に持たせている。

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

**プリセット外の値を使うときは φM と sys_clk の両方を整数分周になる組で指定すること**
（片方だけの変更は小数分周になり、φM にジッタが乗る）。理由と、整数分周にならなかった
場合の挙動は [docs §2](docs/pico-opm-writer.md#2-φm-の生成) にある。

φM を変えても PCM のサンプリングレート（φM/64）が変わるだけで、タイミング定数の
書き換えは要らない。

## 3. ホストインタフェース

### 3.1 接続

- USB CDC (仮想 COM ポート) **2 ポート**。

  | | 用途 | macOS でのデバイス名の例 |
  | --- | --- | --- |
  | CDC #0 | コマンド。Pico SDK の `stdio_usb` がそのまま使う | `/dev/cu.usbmodem112101` |
  | CDC #1 | PCM データ出力（[§4](#4-pcm-出力)） | `/dev/cu.usbmodem112103` |

  CDC #0 をディスクリプタの先頭に置いているので、コマンド側のデバイス名は 1 ポート構成の
  ときと変わらない。CDC #1 は末尾の番号が増えたものになる。
  2 本は独立しているので、コマンドを打ちながら PCM を取り込める。
- CDC を 2 本にする都合で **`picotool load -fx`（BOOTSEL 不要の書き込み）は使えない**
  （[§6.4](#64-書き込み代替経路)）。
- ボーレート・パリティ等の設定は無視される（USB CDC のため何を指定しても動作する）。
- macOS 例: `screen /dev/cu.usbmodem112101 115200`。
  **`/dev/tty.*` ではなく `/dev/cu.*` を使う**（`tty.*` は DCD 待ちで open がブロックする）。
  デバイス名の引き直し方は [§6.5](#65-シリアル-stdio-の読み取り) を参照。
- 接続を検出した時点で起動バナーを出力する（接続前の出力は捨てられるため）。

### 3.2 行フォーマット

- コマンドは **1 行 1 コマンド**。行末は `LF` / `CR` / `CRLF` のいずれでも受け付ける。
- 1 行の最大長は 255 文字。超えた場合は `ERR too long` を返し、その行を破棄する。
- コマンド文字・16 進数の大文字小文字は区別しない。
- トークン区切りは空白またはタブ（連続可）。
- 空行、および **行頭が `#` の行**はコメントとして無視し、**応答も返さない**。

**行末コメント（`w 20 c7 # ...`）には対応していない。** `#` は引数のトークンとして読まれ、
`ERR bad argument` または `ERR wrong arity` になる。行末コメント・プレースホルダ置換・
キャプチャ制御が要るシーケンスは [tools/opm-writer.py](docs/opm-writer.md) 経由で流し込む。

### 3.3 応答

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

### 3.4 コマンド一覧

| コマンド | 書式 | 説明 |
| --- | --- | --- |
| `w` | `w <addr> <data> [<addr> <data> ...]` | レジスタ書き込み。引数は 16 進。addr/data のペアを 1 行に複数並べて連続書き込みできる。**途中でエラーになった場合、そこまでの書き込みは実行済み**のまま `ERR` を返す |
| `r` | `r` | /IC によるハードウェアリセット |
| `c` | `c` | ソフトウェアによる全レジスタクリア（[§3.5](#35-cクリアが書き込む内容)） |
| `d` | `d <ms>` | 指定ミリ秒待機。10 進、`0`-`60000`。待っている間も PCM の送出は続く |
| `p` | `p 1` / `p 0` | PCM 出力の開始 / 停止（[§3.10](#310-ppcm-出力)） |
| `s` | `s` / `s 0` | 統計の表示 / リセット（[§3.11](#311-s統計)） |
| `t` | `t` | 自己テスト（[§3.12](#312-t自己テスト)） |
| `i` | `i` | 情報表示（[§3.6](#36-i情報表示の出力例)） |
| `h` | `h` / `?` | コマンド一覧を表示 |

16 進引数の桁数は自由で、値が `0xff` 以下なら受理する（`w f 1` も `w 0020 00c7` も通る）。
`0xff` を超えると `ERR out of range`。

1 レジスタ書き込みには約 32µs かかる（最大 3 万回/秒）。内訳は
[docs §3.1](docs/pico-opm-writer.md#31-タイミング定数)。

### 3.5 `c`（クリア）が書き込む内容

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

### 3.6 `i`（情報表示）の出力例

```
# pico-opm-writer 0.2.0
# sys_clk : 144000000 Hz
# phiM    : 4000000 Hz (clkdiv 18 + 0/256)
# pins    : D0-D7=GP2-GP9 A0=GP10 /CS=GP11 /WR=GP12 /RD=GP13 /IC=GP14
# pins    : phiM=GP15 /IRQ=GP16
# timing  : t_wr=1us t_addr=5us t_data=25us
# ym3012  : SO=GP17 phi1=GP18 SH1=GP19 SH2=GP20
# capture : ring 16384 bytes (4096 frames) rate 62500 Hz
# i2s     : BCK=GP26 LRCK=GP27 DIN=GP28 (clkdiv 36 + 0/256)
# i2s     : 32fs bck 2000000 Hz rate 62500 Hz latency 1024 frames (16384 us)
# selftest: pio SKIP (disabled)
OK
```

### 3.7 起動バナー

USB CDC の接続を検出した時点で `i` と同じ内容を出力する（末尾の `OK` を含む）。

### 3.8 使用例

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

### 3.9 LED

基板上 LED (`PICO_DEFAULT_LED_PIN`、pico2 では GP25) で状態を示す。表示は 100ms
スロットのパターン駆動で、`sleep_ms()` などで処理を止めることはない。

| 状態 | パターン | 意味 |
| --- | --- | --- |
| 待機 | 常時点灯 | 電源が入っていて、キャプチャしておらず、エラーでもない |
| キャプチャ中 | 500ms ON / 500ms OFF | `p 1` から `p 0` の完了まで |
| コマンド受信 | 100ms ON → 100ms OFF → 100ms ON | 1 回だけ差し込む。終わったら元のパターンの先頭へ戻る |
| DMA overrun | 100ms ON/OFF ×3 → 1 秒 OFF | [§4.1](#41-取りこぼしたとき-dma-overrun)。エラー表示はこれだけ |

キャプチャ中の 500ms 点滅とコマンド受信の 100ms 二重点滅は速さで区別できる。

**LED をエラー表示にするのは DMA overrun のときだけ。** `p` の状態エラー・CDC #1 の切断・
USB 未接続・一時的な USB 送信不可は、いずれもエラー表示にしない。

### 3.10 `p`（PCM 出力）

CDC #1 への PCM 送信を制御する。**取り込み側の PIO と DMA は起動時から常時動いていて、
このコマンドでは開始も停止もしない**（[§4](#4-pcm-出力)）。

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

### 3.11 `s`（統計）

`s` で実行時の統計を表示し、`s 0` でリセットする。単位はすべてバイト。

```
# state   : CAPTURING
# CPU     : 58% (max 58%)
# RING    : 4/16384 bytes  MAX 108/16384  FREE 16380
# USB_TX  : 0/4096 bytes  MAX 100/4096
# I2S     : depth 1024/1024 frames  MIN 1012  UNDERRUN 0
# OVERRUN : 0   E0 : 0   RXSTALL : 0
# RATE    : 62500 frames/s (expect 62500)
# FRAMES  : 56263161
# PIOTEST : SKIP (disabled)
# IRQ     : H
OK
```

| 項目 | 内容 |
| --- | --- |
| `state` | `IDLE` / `CAPTURING` / `DRAINING` / `ERROR` |
| `CPU` | 直近 1 秒の Core 0 使用率と、リセット以降の最大値。USB の空き待ちで何も送れなかった周回は idle として数える |
| `RING` | DMA リングの未処理量と high-water、空き |
| `USB_TX` | CDC #1 の送信バッファ滞留量と high-water。USB エンドポイントの状態ではなく、ファーム内の TX FIFO の滞留量 |
| `I2S` | I2S の DMA より先に書けているフレーム数と low-water、アンダーラン回数（[§5](#5-i2s-出力)）。ここだけは減る方向が危険なので最小値を残す |
| `OVERRUN` | DMA overrun の発生回数 |
| `E0` | YM3012 の禁止コード `E=0` を見た数。**PIO のビット位相が正しければ 0 のまま** |
| `RXSTALL` | PIO の RX FIFO があふれた回数。あふれると L/R の並びが崩れる |
| `RATE` | 直近 1 秒で数えた実測フレームレートと期待値 φM/64 |
| `FRAMES` | 取り込んだ総フレーム数 |
| `PIOTEST` | 起動時の PIO ループバック自己診断の結果（[docs §4.6](docs/pico-opm-writer.md#46-起動時の自己診断)）。既定では `SKIP (disabled)`（[§5.4](#54-無効化)） |
| `IRQ` | OPM の /IRQ の現在のレベル |

`E0` / `RXSTALL` / `RATE` は、ロジックアナライザを繋がずにキャプチャ経路の健全性を
確かめるための指標。

### 3.12 `t`（自己テスト）

外部機器を使わずに実行できる自己テストをまとめて走らせる。

```
> t
# pcm     : PASS
# pio     : SKIP (disabled)
OK
```

| 項目 | 内容 |
| --- | --- |
| `pcm` | PCM 変換の既知ベクタ検証。ゼロ / ±1 / 仮数境界 / 指数全域 / 禁止コード / 無効 3bit のマスク / 値域の両端。加えて全 `E`・全仮数でステップが `1 << (E-1)` になることを総当たりで確認する |
| `pio` | 起動時に実施した PIO ループバック自己診断の結果（[docs §4.6](docs/pico-opm-writer.md#46-起動時の自己診断)）。**I2S が有効な既定構成では GP26-GP28 が競合するので実施せず `SKIP (disabled)` になる**（[§5.4](#54-無効化)） |

どちらかが失敗したら `ERR self test failed` を返す。

## 4. PCM 出力

OPM が YM3012 (DAC) へ送るシリアル出力を取り込み、PCM に変換して CDC #1 へ流す。
`p 1` / `p 0`（[§3.10](#310-ppcm-出力)）で送信を制御する。

| 項目 | 値 |
| --- | --- |
| 形式 | signed 16bit little-endian、ステレオインタリーブ（L, R, L, R, …） |
| サンプリングレート | **φM / 64**（既定の φM 4MHz なら 62500 Hz） |
| 値域 | **−32768 〜 +32704** |
| チャネル | YM3012 の CH1 = **L**、CH2 = **R** |

- ヘッダは付かない。CDC #1 を開いて読んだバイト列がそのまま PCM になる。
- **正側が +32767 に届かないのは YM3012 の仕様どおり**（最大値は `511 << 6 = 32704`）。
  正規化してはいけない。
- サンプリングレートは φM に比例するので、`OPM_CLOCK_MODE_NTSC`（φM 3.579545MHz）では
  55930.4 Hz になる。WAV に書き出すときはこのレートを使う。

変換の仕組みとフレーム構造は
[docs §4](docs/pico-opm-writer.md#4-ym3012-dac-キャプチャ) にある。

### 4.1 取りこぼしたとき (DMA overrun)

**取り込みは常時動いていて、ファーム内のリングバッファは 65.5ms 分しかない**（φM 4MHz で
4096 フレーム）。`p 1` の間、ホストは CDC #1 を読み続ける必要がある。

読み落としてリングが一杯になると、壊れたデータを送り続けないよう次のように振る舞う。

1. 送信を停止する（`s` の `state` が `ERROR` になる）
2. コマンド側の CDC #0 へ `# ERR dma overrun` を出す
3. LED をエラー表示にする（[§3.9](#39-led)）
4. 次の `p 1` を受けるまで止まったまま

`# ` 始まりの情報行として出すので、「1 コマンド 1 応答」（[§3.3](#33-応答)）は崩れない。
発生回数は `s` の `OVERRUN` に残る。次の `p 1` でそのまま復帰できる。

[tools/opm-writer.py](docs/opm-writer.md) の `!capture` を使えば、読み出しは
スクリプト側が面倒を見る。

## 5. I2S 出力

キャプチャした PCM をそのまま I2S で外部 DAC (PCM5102A) へ流す。**電源を入れれば常に
出力していて、開始も停止もコマンドは無い**。レジスタを書けばその場で音が出る。

USB キャプチャ（[§4](#4-pcm-出力)）とは独立した読み出し位置を持つので、`p 1` で
キャプチャしながら鳴らしても互いに干渉しない。

### 5.1 DAC の配線

| Pico 2 | PCM5102A | 備考 |
| --- | --- | --- |
| GP26 | BCK | ビットクロック |
| GP27 | LRCK / WS | ワードセレクト。**GP26 の次の GPIO であること**（PIO の sideset 2bit に載せる） |
| GP28 | DIN | データ |
| GND | SCK | **GND へ落とす。** SCK を L にすると内蔵 PLL が BCK からシステムクロックを作る |
| — | XSMT | H 固定（ソフトミュートは使わない）。多くのブレークアウト基板は既定で H |

MCLK (SCK) を DAC 内部で作らせるので、Pico から出すのは 3 本だけ。

### 5.2 フォーマット

| 項目 | 値 |
| --- | --- |
| フォーマット | Philips 標準 I2S、MSB first、16bit |
| BCK | 32fs（φM 4MHz で 2.000 MHz） |
| サンプリングレート | φM/64 = 62500 Hz（`OPM_CLOCK_MODE_NTSC` では 55930.4 Hz） |
| チャンネル | LRCK=0 が L (YM3012 CH1) / LRCK=1 が R (CH2) |
| レイテンシ | 1024 フレーム = 16.4 ms |

62.5kHz は標準的なレートではないが、PCM5102A は内蔵 PLL が BCK に追従するので
そのまま鳴る（対応範囲 8k〜384kHz）。

**サンプリングレートは変換していない。** φM も I2S も同じ sys_clk から分周しているため、
キャプチャ側と出力側のレートは厳密に一致する。リサンプリングもドリフト補正も無く、
固定長のバッファを挟むだけで済む。分周比の導出は
[docs §5.2](docs/pico-opm-writer.md#52-サンプリングレートのロック)。

L と R は同じ波形にならない（[test/dac_lr/](test/dac_lr/README.md)）。これは YM3012 の
仕様どおりで、I2S 出力もそのまま両チャンネルを出している。

### 5.3 アンダーラン

I2S へ供給する DMA は止まらないので、CPU がリングを埋め遅れると **DMA は無音ではなく
古いリング内容を再生する**。検出したら未処理を捨てて先行分を無音で埋め直し、
`s` の `UNDERRUN` を 1 増やす（[§3.11](#311-s統計)）。一瞬ノイズが出るが継続はしない。

余裕は 16.4ms あるので、通常の動作では起きない。`s` の `I2S` の `MIN`（先行量の
low-water）がどこまで削れたかの指標になる。

OPM を繋がずに動かした場合はソースが供給されないため、16.4ms ごとにアンダーランを
繰り返す。出力は無音のままで、`UNDERRUN` だけが増え続ける。

### 5.4 無効化

`I2S_ENABLED=0` で再コンフィグすると I2S を止め、GP26-GP28 を解放できる。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 -DI2S_ENABLED=0
```

このとき起動時の PIO ループバック自己診断が自動的に有効になる
（[docs §4.6](docs/pico-opm-writer.md#46-起動時の自己診断)）。診断は GP26-GP28 を使うので
I2S とは同時に使えず、**I2S が有効な既定構成では `t` / `s` の `pio` は
`SKIP (disabled)` になる**。DAC を外して診断だけ試したいときは
`-DYM3012_LOOPBACK=1` を付ける。

## 6. ビルドと書き込み

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

### 6.1 ビルド

```bash
# 増分ビルド（通常はこれだけで足りる）
ninja -C build

# 再コンフィグ（CMakeLists.txt や .pio を変更したとき）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2
```

`build/` は Release / `rp2350-arm-s` で構成済み。`.vscode/tasks.json` の "Compile Project"
タスクが増分ビルドと等価。

成果物は `build/pico-opm-writer.{uf2,elf,bin,hex,dis}` と `build/compile_commands.json`。

### 6.2 書き込み（SWD / PicoProbe が既定）

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

### 6.3 デバッグ

GUI でのステップ実行デバッグは VS Code の "Pico Debug (Cortex-Debug)" 構成を使う。

### 6.4 書き込み（代替経路）

SWD を繋いでいない場合は BOOTSEL から書き込む。BOOTSEL を押しながら USB を挿し、
現れる RPI-RP2 ドライブへ `build/pico-opm-writer.uf2` をコピーする。

```bash
picotool load build/pico-opm-writer.uf2   # BOOTSEL で起動した状態で実行する
```

**`picotool load -fx`（BOOTSEL 操作なしで再起動させる書き込み）は使えない。**
CDC を 2 本にするため TinyUSB のディスクリプタを自前で持っており、
`PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE` が無効になるため
（[docs §6](docs/pico-opm-writer.md#6-usb-cdc-2-本構成の実装)）。

### 6.5 シリアル (stdio) の読み取り

| デバイス | 中身 |
| --- | --- |
| `/dev/cu.usbmodem112101` | **ターゲット pico2 の USB CDC #0** = コマンド / `printf` の出力先 |
| `/dev/cu.usbmodem112103` | **ターゲット pico2 の USB CDC #1** = PCM データ出力（[§4](#4-pcm-出力)） |
| `/dev/cu.usbmodem112202` | PicoProbe 側の CDC-UART ブリッジ（現状の設定では未使用） |

tty 名は USB のポート位置に依存する。変わったら `ioreg -r -c IOSerialBSDClient -l -w 0` の
`locationID` と `ioreg -p IOUSB -l -w 0` の `USB Product Name` を突き合わせて引き直す
（ターゲットは Product Name `Pico`、プローブは `Debugprobe on Pico _CMSIS_DAP_`）。

注意点:

- **`/dev/tty.*` ではなく `/dev/cu.*` を使う。** `tty.*` は DCD 待ちで open がブロックする。
- **SWD の `reset` でターゲットの USB CDC が切断・再列挙される。** 書き込み直後は
  デバイスノードが数秒消えるので、存在を待ってから開く。
- `stdio_usb` はホストが開く前の出力を捨てるため、起動直後の行は取り逃す。

## 7. ホスト側ツール

`tools/` にホスト PC 側のスクリプトを置いている。いずれも Python 3 の標準ライブラリだけで
動く（`.zst` を扱う機能のみ Python 3.14 以上が必要）。

| スクリプト | 役割 | ドキュメント |
| --- | --- | --- |
| `tools/opm-writer.py` | シーケンスファイルを USB CDC 経由でファームへ流し込む。行末コメント / `@KEY@` 置換 / `!capture` によるキャプチャに対応。`!capture` はファームの CDC #1 から PCM をキャプチャする | [docs/opm-writer.md](docs/opm-writer.md) |
| `tools/opm-lfo-period.py` | 実機キャプチャから LFO の更新周期をサンプル数で測る（`--mode am` / `--mode pm`）。結果は 1 ファイル 1 行の TSV | [docs/opm-lfo-period.md](docs/opm-lfo-period.md) |
| `tools/opm-lfo-period-testgen.py` | `opm-lfo-period.py` の回帰テスト（実機不要） | [docs/opm-lfo-period-testgen.md](docs/opm-lfo-period-testgen.md) |

**キャプチャはファームの CDC #1 から PCM を直接読むので、ロジックアナライザは要らない。**
`test/` の掃引スクリプトはそのまま実行できる。

```bash
./test/dac_lr/capture_all.py --analyze
```

`test/` 以下は、これらを使った実機調査の一次データ生成環境。掃引スクリプトと測定条件は
それぞれの README にまとめてある。

| ディレクトリ | 調べていること |
| --- | --- |
| [test/lfo_noise/](test/lfo_noise/README.md) | LFO ノイズ波形（`LFRQ` / `NFRQ` の掃引） |
| [test/noise_period/](test/noise_period/README.md) | ノイズ発生器そのもの（NE でノイズを直接 DAC へ出す） |
| [test/dac_lr/](test/dac_lr/README.md) | DAC の 2 スロット (CH1/CH2) の関係 |

## 8. 将来の拡張（本仕様の範囲外）

- バイナリストリーミングモード（`w` の 1 行あたりのオーバーヘッド削減）
- 時刻付きレジスタ列の一括転送とファーム側タイマによる再生（VGM 再生の下地）
- `/RD` を使ったステータス（BUSY）ポーリングによる待ち時間の最適化。配線と
  データバスの向き切り替え（`opm_bus_set_dir()`）は用意してあるが、読み出し自体は未実装
- `/IRQ` の割り込み処理。現在はレベルを参照できるだけ（`s` の `IRQ`）
- バス書き込みの PIO 化と FIFO による非同期キューイング
- 2 個目の OPM / 他の Yamaha 音源チップ (OPN 系) への対応

## 9. ライセンス

MIT License。詳細は [LICENSE](LICENSE) を参照。

ただし `pico_sdk_import.cmake` は Raspberry Pi (Trading) Ltd. 由来のファイルで、
BSD-3-Clause が適用される（ファイル先頭の表示のとおり）。
