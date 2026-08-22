# pico-opm-writer

Raspberry Pi Pico 2 (RP2350) を YM2151 (OPM) に直結し、ホスト PC から USB シリアル経由で
OPM のレジスタへ値を書き込むためのファームウェア。

ホストからは USB CDC (仮想 COM ポート) として見え、`screen` や `minicom` などの素の
ターミナルからテキストコマンドを打つだけでレジスタを操作できる。バッチ実行や
DAC 出力のキャプチャには [ホスト側ツール](#10-ホスト側ツール)（`tools/`）を使う。

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
意味を持たないため、両者をペアにしたプリセットとして持たせている。

| プリセット | `clock` の引数 | φM | sys_clk | clkdiv | fs (φM/64) | 備考 |
| --- | --- | --- | --- | --- | --- | --- |
| 4MHz（**起動時の既定**） | `4` | 4.000000 MHz | 144 MHz（= 4MHz × 36） | 18 | 62500.0 Hz | YM2151 の定格上限。RP2350 定格 150MHz 以内 |
| NTSC | `3.58` | 3.579545 MHz | 157.5 MHz（= × 44） | 22 | 55930.4 Hz | NTSC カラーサブキャリア = 315/88 MHz。定格比 約 +5% の OC |

**動作させたまま切り替えられる。**

- `clock 4` / `clock 3.58` で手動切り替え（[§3.16](#316-clockクロック切り替え)）
- `vgm play` はヘッダが申告する YM2151 のクロックへ自動追従する
  （[§8.3](#83-φm-と-vgm-のクロック)）。`clock fixed` で抑止できる
- `mdx play` は X68000 の φM = 4MHz へ寄せる（[§9.3](#93-φm-と-テンポ)）。
  同じく `clock fixed` で抑止できるが、MDX は**テンポも φM に比例して狂う**
- **PCM キャプチャ中（`p 1`）は切り替えを拒否する。** サンプリングレートが
  ストリームの途中で変わると、ホスト側で出来上がる WAV の時間軸が黙って狂うため

切り替えは sys_clk ごと張り替えるが、**φM・BCK・LRCK の H/L 期間が公称値より
短くなることはない**（順序と手順は [docs §2.3](docs/pico-opm-writer.md#23-実行時の切り替え)）。
所要は数百 µs で、その間 I2S のリング（先行 16.4ms）は枯れないのでアンダーランしない。

起動時にどちらで立ち上げるかは次のどちらかで決める。

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
| `ERR unknown command` | 未知のコマンド。1 文字のコマンドか `storage` / `vgm` のいずれでもない場合と、`storage` / `vgm` のサブコマンドが未知の場合 |
| `ERR bad argument` | 引数が 16 進数 / 10 進数として解釈できない、またはファイル名が不正 |
| `ERR wrong arity` | 引数の個数が合わない |
| `ERR out of range` | 引数が許容範囲外 |
| `ERR too long` | 行が長すぎる |
| `ERR wrong state` | いまの状態では実行できない（[§3.13](#313-storageストレージ) / [§3.14](#314-vgmvgm-再生)）。直前に理由を示す `# hint` 行が出る |
| `ERR no filesystem` | ストレージが未フォーマット、または領域がファームウェアと重なっている |
| `ERR not found` | 指定した VGM ファイルまたは `/VGM` が無い |
| `ERR bad file` | VGM として読めない（マジック不正 / ヘッダが壊れている / gzip ストリームが壊れている） |
| `ERR io error` | フラッシュまたはファイルシステムの入出力に失敗した |

### 3.4 コマンド一覧

| コマンド | 書式 | 説明 |
| --- | --- | --- |
| `w` | `w <addr> <data> [<addr> <data> ...]` | レジスタ書き込み。引数は 16 進。addr/data のペアを 1 行に複数並べて連続書き込みできる。**途中でエラーになった場合、そこまでの書き込みは実行済み**のまま `ERR` を返す |
| `r` | `r` | /IC によるハードウェアリセット。I2S のアンダーランを 1 回伴う（[§5.3](#53-アンダーラン)） |
| `c` | `c` | ソフトウェアによる全レジスタクリア（[§3.5](#35-cクリアが書き込む内容)） |
| `d` | `d <ms>` | 指定ミリ秒待機。10 進、`0`-`60000`。待っている間も PCM の送出は続く |
| `p` | `p 1` / `p 0` | PCM 出力の開始 / 停止（[§3.10](#310-ppcm-出力)） |
| `s` | `s` / `s 0` | 統計の表示 / リセット（[§3.11](#311-s統計)） |
| `t` | `t` | 自己テスト（[§3.12](#312-t自己テスト)） |
| `i` | `i` | 情報表示（[§3.6](#36-i情報表示の出力例)） |
| `h` | `h` / `?` | コマンド一覧を表示 |
| `clock` | `clock` / `clock 4` / `clock 3.58` / `clock auto` / `clock fixed` | φM の表示と切り替え（[§3.16](#316-clockクロック切り替え)） |
| `storage` | `storage status` / `host` / `player` / `format yes` / `trace` | ストレージの状態表示とモード切り替え（[§3.13](#313-storageストレージ)） |
| `vgm` | `vgm list` / `vgm play <filename>` / `vgm stop` | VGM の一覧と再生（[§3.14](#314-vgmvgm-再生)） |
| `mdx` | `mdx list` / `mdx play <filename>` / `mdx stop` | MDX の一覧と再生（[§3.15](#315-mdxmdx-再生)） |

16 進引数の桁数は自由で、値が `0xff` 以下なら受理する（`w f 1` も `w 0020 00c7` も通る）。
`0xff` を超えると `ERR out of range`。

1 文字のコマンドは大小どちらでもよい。`storage` / `vgm` / `mdx` とそのサブコマンドも同様。
`vgm play` と `mdx play` のファイル名だけは行の残り全部を 1 引数として受けるので、
空白を含む名前もそのまま書ける（`vgm play BAD NAME.VGM`）。

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
# preset  : 4  (vgm auto)
# pins    : D0-D7=GP2-GP9 A0=GP10 /CS=GP11 /WR=GP12 /RD=GP13 /IC=GP14
# pins    : phiM=GP15 /IRQ=GP16
# timing  : t_wr=1us t_addr=5us t_data=25us
# ym3012  : SO=GP17 phi1=GP18 SH1=GP19 SH2=GP20
# capture : ring 16384 bytes (4096 frames) rate 62500 Hz
# i2s     : BCK=GP26 LRCK=GP27 DIN=GP28 (clkdiv 36 + 0/256)
# i2s     : 32fs bck 2000000 Hz rate 62500 Hz latency 1024 frames (16384 us)
# selftest: pio SKIP (disabled)
# storage : flash 0x200000 + 2048 KiB  cluster 4096 B  sector 512 B
# vgm     : dir /VGM  rate 44100 Hz  budget 500 us
# mdx     : dir /MDX  max 64 KiB  budget 500 us
# adpcm   : enabled  8 ch  pdx stream 1024 bytes/ch
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
# LOOP    : 172376 passes/s
# FRAMES  : 56263161
# FLASH   : WRITE 76   BLACKOUT max 42711 us
# VGM     : PLAYING AFTERBURNER.VGM
# VGM POS : 507150/2205000 samples  loop 7
# VGM LAG : reslip 0  gz reload 0
# MDX     : STOPPED
# MDX POS : 0 clocks  loopjump 0  ch 0
# MDX TICK: @t 200  14336 us  reslip 0
# MDX PCM : on  1/8 ch  mask 01  keyon 128  miss 0  reads 647  CLIP 2
# SEQ LAG : max 657 us
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
| `LOOP` | 直近 1 秒のメインループ周回数。1 周あたりの固定費を見積もるのに使う |
| `FRAMES` | 取り込んだ総フレーム数 |
| `FLASH` | 内蔵フラッシュへ 4KiB ブロックを書き出した回数と、その間メインループが止まった最大時間（[§3.13](#313-storageストレージ)） |
| `VGM` | VGM の再生状態（`STOPPED` / `PLAYING` / `ERROR`）と再生中のファイル名。gzip 圧縮されたファイルなら末尾に `(gzip)` が付く（[§8.5](#85-vgzgzipの再生)） |
| `VGM POS` | 発行済みのサンプル位置 / ヘッダの総サンプル数と、ループした回数 |
| `VGM LAG` | VGM の時計を張り直した回数（[§3.14](#314-vgmvgm-再生)）。`gz reload` は `.vgz` のループで先頭から展開し直した回数で、**0 でなければループのたびに音が数百 ms 途切れている**（[§8.5](#85-vgzgzipの再生)） |
| `MDX` | MDX 再生の状態とファイル名（[§3.15](#315-mdxmdx-再生)） |
| `MDX POS` | 発行済みの clock 数、ループジャンプの回数、チャンネル数。`loopjump` は全チャンネルの合計なので「曲が何周したか」ではない |
| `MDX TICK` | 現在の Timer-B 値と 1 clock の長さ、時計を張り直した回数（[§9.2](#92-タイミング)） |
| `MDX PCM` | ADPCM ミキシングの有効・無効、発音中のチャンネル数とビットマスク、発音を開始した回数（`keyon`）と鳴らせなかった回数（`miss`）、PDX を読んだ回数、FM に足した結果あふれたサンプル数（[§9.7](#97-adpcm-pcm8-の再生)） |
| `SEQ LAG` | シーケンサが予定時刻から遅れた最大時間。**VGM と MDX で共用**（同時には再生できないので 1 個で足りる） |
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
# mdx     : PASS (7 pitch, 5 tempo)
# adpcm   : PASS (19 cases)
OK
```

| 項目 | 内容 |
| --- | --- |
| `pcm` | PCM 変換の既知ベクタ検証。ゼロ / ±1 / 仮数境界 / 指数全域 / 禁止コード / 無効 3bit のマスク / 値域の両端。加えて全 `E`・全仮数でステップが `1 << (E-1)` になることを総当たりで確認する |
| `pio` | 起動時に実施した PIO ループバック自己診断の結果（[docs §4.6](docs/pico-opm-writer.md#46-起動時の自己診断)）。**I2S が有効な既定構成では GP26-GP28 が競合するので実施せず `SKIP (disabled)` になる**（[§5.4](#54-無効化)） |
| `mdx` | MDX の音程 → KC/KF 変換と、Timer-B 値 → 1 clock の長さの既知ベクタ検証。KC の下位 4bit が 3/7/11/15 を飛ばす境界とオクターブ跨ぎを含む（[§9.4](#94-音程と音量の作り方)） |
| `adpcm` | MSM6258 の ADPCM デコーダの既知ベクタ検証。ステップ幅の表・符号ビット・12bit の飽和・段番号の上下端での頭打ちを含む。加えてレート比が全モードで整数であることと、音量 8 が原音（ゲイン 1.0）であることを確かめる（[§9.7](#97-adpcm-pcm8-の再生)） |

どれかが失敗したら `ERR self test failed` を返す。

### 3.13 `storage`（ストレージ）

内蔵フラッシュ後半の FAT ファイルシステムを、**Pico 側（FatFs）と PC 側（USB MSC）の
どちらが持つか**を排他で切り替える。詳しくは [§7](#7-ストレージ) を参照。

| コマンド | 説明 |
| --- | --- |
| `storage status` | 現在の状態を表示する |
| `storage host` | フラッシュを PC へ渡す。PC にリムーバブルディスクとして現れる |
| `storage player` | フラッシュを Pico 側へ戻す。FatFs をマウントし直す |
| `storage format yes` | 領域を作り直す。既にファイルシステムがある場合は `storage format force yes` |
| `storage trace` | 直前に PC が投げた SCSI コマンドの記録を表示する（[§7.4](#74-マウントされないときの調べ方)） |

`storage status` の出力例:

```
# storage : PLAYER
# medium  : not present
# audio   : enabled
# region  : flash 0x200000 + 2048 KiB  (LBA 512 B x 4096)
# firmware: end 0x10011130 (69936 B)  gap 1979 KiB
# fs      : FAT12  cluster 4096 B  free 1960/2028 KiB
# label   : OPMVGM
# cache   : 8 lines  dirty 0
# flash   : WRITE 25   BLACKOUT max 40693 us
OK
```

| 項目 | 内容 |
| --- | --- |
| `storage` | `PLAYER`（Pico が持つ） / `HOST`（PC が持つ） |
| `medium` | MSC にメディアが入っていると見せているか。`HOST` のときだけ `present` |
| `audio` | PCM キャプチャと I2S 出力が使えるか。`HOST` 中は無効 |
| `region` | フラッシュ上の領域と論理セクタの構成 |
| `firmware` | ファームウェア末尾のアドレスと、領域先頭までの余裕 |
| `fs` | FAT の種類・クラスタ長・空き容量。未フォーマットなら `no filesystem` |
| `label` | ボリュームラベル |
| `cache` | ライトバックキャッシュの行数と、未書き出しの行数 |
| `flash` | 4KiB ブロックの書き出し回数と、その間メインループが止まった最大時間 |

**`storage host` は、VGM 再生中か PCM キャプチャ中だと `ERR wrong state` で拒否する。**
どちらのガードで落ちたかは直前の `# hint` 行に出る。先に `vgm stop` / `p 0` を実行すること。

`HOST` 中は次のものが使えない。フラッシュの消去でメインループが数十 ms 止まり、
キャプチャの DMA リング（65.5ms 分）と I2S の先行量（16.4ms 分）を守れないため。

* `p 1`（PCM キャプチャ）
* I2S 出力（無音になる。BCK / LRCK は止めないので DAC はポップしない）
* `vgm list` / `vgm play`（ファイルシステムがアンマウントされている）
* `storage format`

`PLAYER` へ戻した時点で音声経路を復帰させ、キャプチャと I2S のリング位置を張り直す。

### 3.14 `vgm`（VGM 再生）

`/VGM/` に置いた VGM ファイルを再生する。演奏対象は **YM2151 の部分だけ**で、
他の音源のコマンドは読み飛ばす。gzip 圧縮された `.vgz` も一時ファイルを作らずに
そのまま再生できる（[§8.5](#85-vgzgzipの再生)）。詳しくは [§8](#8-vgm-再生) を参照。

| コマンド | 説明 |
| --- | --- |
| `vgm list` | `/VGM/` の `.vgm` と `.vgz` を名前順（大小無視）に並べる |
| `vgm play <filename>` | `/VGM/<filename>` を再生する。`/VGM/` は付けない |
| `vgm stop` | 再生を止めて全チャンネルをキーオフする |

```
> vgm list
# file    :   1234567 AFTERBURNER.VGM
# file    :    234567 OUTRUN.VGZ
# files   : 2
OK

> vgm play AFTERBURNER.VGM
# vgm     : version 1.51  samples 2205000  loop yes
# clock   : file 3579545 Hz / phiM 4000000 Hz (音程が高くなる)
OK
```

サイズを先に置き、ファイル名を必ず最後の欄にしてある（名前に空白を含みうるため）。
`.vgz` のサイズは圧縮された状態のバイト数。

`.vgz` を再生すると `# vgm` 行の末尾に `gzip` が付く。

```
> vgm play OUTRUN.VGZ
# vgm     : version 1.51  samples 1852000  loop yes  gzip
OK
```

**再生中は `w` / `r` / `c` と `storage host` を `ERR wrong state` で拒否する。**
VGM とユーザーのレジスタ書き込みが混ざると何が鳴っているのか分からなくなるため。
`p 1` / `p 0` / `s` / `i` / `t` / `d` は再生中も使える（VGM を鳴らしながら
CDC #1 へ録音できる）。

再生を始めたあとにファイルの中身が壊れていると分かった場合は、`OK` を返したあとなので
非同期通知になる。状態は `ERROR` になり `s` から見える。

```
# ERR vgm bad file (opcode 0x2f at 0x0001a34c)
```

### 3.15 `mdx`（MDX 再生）

`/MDX/` に置いた MDX ファイル（X68000 の MXDRV 用のバイナリ MML）を再生する。
**FM 8ch と ADPCM 8ch** を鳴らす。ADPCM は本機にハードウェアが無いのでソフトウェアで
デコードし、FM の出力に足して I2S と PCM キャプチャへ流す。
詳しくは [§9](#9-mdx-再生) を参照。

| コマンド | 説明 |
| --- | --- |
| `mdx list` | `/MDX/` の `.mdx` を名前順（大小無視）に並べる |
| `mdx play <filename>` | `/MDX/<filename>` を再生する。`/MDX/` は付けない |
| `mdx stop` | 再生を止めて全チャンネルをキーオフする |
| `mdx pcm` | ADPCM ミキシングの状態を表示する |
| `mdx pcm on` / `mdx pcm off` | ADPCM を足す / 足さない（FM だけの音と聴き比べる用） |

```
> mdx list
# file    :      8192 GRADIUS.MDX
# file    :     12345 XEVIOUS.MDX
# files   : 2
OK

> mdx play GRADIUS.MDX
# mdx     : GRADIUS.MDX
# title   : グラディウス / KONAMI
# ch      : 9  voices 12
OK
```

タイトルはファイルに入っている Shift_JIS をそのまま流す。UTF-8 の端末では化けるので、
必要なら端末側で文字コードを切り替える。`ch` は 9（FM 8 + ADPCM 1）か
16（FM 8 + ADPCM 8。PCM8 拡張）のどちらか。

PDX（ADPCM の波形集）を要求する曲では、その名前と、実際に開いたファイルが出る。

```
> mdx play THEXDER.MDX
# mdx     : THEXDER.MDX
# title   : THEXDER
# ch      : 9  voices 20
# pdx     : THEXDER
# adpcm   : /MDX/THEXDER.PDX
OK
```

PDX が見つからないときはエラーにはならず、FM パートだけがそのまま鳴る。

```
# pdx     : THEXDER
# hint    : /MDX/THEXDER.PDX を開けない (not found)。ADPCM パートは鳴らない
```

`mdx pcm` で ADPCM の状態が見える。

```
> mdx pcm
# pcm     : on
# pdx     : /MDX/THEXDER.PDX
# ch      : 2 active  mask 03  pan L+R
# keyon   : 128   miss 0
# reads   : 647   clip 0
OK
```

`keyon` は ADPCM の発音を開始した回数、`miss` は鳴らそうとして波形が見つからなかった
（または音量 0 になった）回数。**ADPCM が終盤にしか出てこない曲もある**ので、
「聞こえない」だけでは曲の側の話かファームの側の話か分からない。この 2 つを見れば
聴かずに切り分けられる。カウンタは `mdx play` で 0 に戻り、曲が終わったあとも残る。

`mdx pcm off` は**電源を切るまで残る**。off のまま PDX を要求する曲を再生すると、
`mdx play` が理由を出す。

```
> mdx play THEXDER.MDX
...
# adpcm   : /MDX/THEXDER.PDX
# hint    : ADPCM のミキシングは off。mdx pcm on で戻す
```

**再生中は `w` / `r` / `c` と `storage host` を `ERR wrong state` で拒否する。**
VGM と MDX を同時に再生することもできない（どちらか一方だけ）。
`p 1` / `p 0` / `s` / `i` / `t` / `d` は再生中も使える。

`.mdx` として読めない中身は `ERR bad file`。64KiB を超えるファイルも受け付けない。
再生を始めたあとに壊れていると分かった場合は `OK` を返したあとなので非同期通知になる。

```
# ERR mdx bad file (truncated at 0x00000c34)
```

全チャンネルが演奏終了（`0xF1 0x00`）に達すると自動で止まる。ループを持つ曲は
止まらないので `mdx stop` で止める。

### 3.16 `clock`（クロック切り替え）

φM を動作させたまま切り替える。プリセットは [§2](#2-クロック設定-φm) の 2 つ。

| 書式 | 動作 |
| --- | --- |
| `clock` | 現在の φM / sys_clk / プリセット / I2S のレートを表示する |
| `clock 4` | φM 4.000000MHz（sys_clk 144MHz）へ切り替える |
| `clock 3.58` | φM 3.579545MHz（sys_clk 157.5MHz）へ切り替える |
| `clock auto` | `vgm play` でファイルのクロックへ追従する（**既定**） |
| `clock fixed` | 追従を止め、手動で選んだプリセットを保つ |

切り替えたときは `clock` と同じ内容を出してから `OK` を返す。

```
> clock 3.58
# phiM    : 3579545 Hz (clkdiv 22 + 0/256)
# sys_clk : 157500000 Hz
# preset  : 3.58  (vgm auto)
# i2s     : clkdiv 44 + 0/256  rate 55930 Hz  bck 1789772 Hz
# capture : rate 55930 Hz
OK
```

既に同じプリセットのときは何もせずに応答だけを返す。

**PCM キャプチャ中（`p 1`）は `ERR wrong state` で拒否する。** サンプリングレートが
ストリームの途中で変わると、ホスト側で出来上がる WAV の時間軸が黙って狂うため。
これは `vgm play` の自動追従にも同じように効き、キャプチャ中に別クロックの VGM を
再生しようとすると再生自体が `ERR wrong state` になる（同じクロックなら通る）。

```
> clock 3.58
# hint    : PCM キャプチャ中は切り替えられない。先に p 0 を実行すること
ERR wrong state
```

VGM 再生中は拒否しない。レジスタを叩くわけではなく、変わるのは音程と包絡線の速さだけで、
テンポは 44100Hz の絶対サンプル数で刻んでいるので狂わない。

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

**`r`（/IC リセット）を実行すると必ず 1 回アンダーランする。** /IC の間は YM3012 が出力を
止めてソースが 10ms ぶん欠け、`opm_reset()` の待ちの間はリングを補充できないので、
先行量 16.4ms を使い切る。リセット直後に一瞬ノイズが出るだけで継続はしない。
**`UNDERRUN` はリセットした回数だけ増えるので、値を見るときは `s 0` でリセットしてからの
差分で見ること。** 内訳は [docs §5.4](docs/pico-opm-writer.md#54-アンダーラン) にある。

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
（[docs §6](docs/pico-opm-writer.md#6-usb-cdc-2-本と-msc-の実装)）。

### 6.5 シリアル (stdio) の読み取り

| デバイス | 中身 |
| --- | --- |
| `/dev/cu.usbmodem112101` | **ターゲット pico2 の USB CDC #0** = コマンド / `printf` の出力先 |
| `/dev/cu.usbmodem112103` | **ターゲット pico2 の USB CDC #1** = PCM データ出力（[§4](#4-pcm-出力)） |
| `/dev/cu.usbmodem112202` | PicoProbe 側の CDC-UART ブリッジ（現状の設定では未使用） |

これに加えて USB マスストレージのインタフェースが 1 本ある。`storage host` のあいだだけ
メディアが入っている状態になり、PC にリムーバブルディスクとして現れる（[§7](#7-ストレージ)）。

tty 名は USB のポート位置に依存する。変わったら `ioreg -r -c IOSerialBSDClient -l -w 0` の
`locationID` と `ioreg -p IOUSB -l -w 0` の `USB Product Name` を突き合わせて引き直す
（ターゲットは Product Name `Pico`、プローブは `Debugprobe on Pico _CMSIS_DAP_`）。

注意点:

- **`/dev/tty.*` ではなく `/dev/cu.*` を使う。** `tty.*` は DCD 待ちで open がブロックする。
- **SWD の `reset` でターゲットの USB CDC が切断・再列挙される。** 書き込み直後は
  デバイスノードが数秒消えるので、存在を待ってから開く。
- `stdio_usb` はホストが開く前の出力を捨てるため、起動直後の行は取り逃す。

## 7. ストレージ

Raspberry Pi Pico 2 に載っている **内蔵 QSPI フラッシュ 4MiB の後半 2MiB** を
FAT ファイルシステムとして使う。外付けのフラッシュや SD カードは足さないので、
**GPIO は 1 本も消費しない**。

```
0x10000000  ┬ ファームウェア（約 70KiB）
            ┆  （空き 約 1.9MiB）
0x10200000  ┬ FatFs 領域 2MiB
0x10400000  ┴
```

| 項目 | 値 |
| --- | --- |
| 領域 | フラッシュ先頭から 0x200000、サイズ 0x200000（2MiB） |
| フォーマット | FAT12 / クラスタ 4096 バイト / SFD（MBR 無し） |
| 論理セクタ | 512 バイト（FatFs と USB MSC で共通） |
| ボリュームラベル | `OPMVGM` |
| VGM の置き場所 | `/VGM/` |

クラスタ長をフラッシュの消去単位と同じ 4096 バイトに揃えてあるので、
**1 クラスタの書き換えがフラッシュの消去 1 回で済む**。

### 7.1 領域の変え方

領域は [flash_disk.h](flash_disk.h) の 2 つの定数だけで決まる。

```c
#define FLASH_FATFS_OFFSET (2u * 1024u * 1024u)   /* XIP_BASE からのオフセット */
#define FLASH_FATFS_SIZE   (2u * 1024u * 1024u)
```

ヘッダを触らずに変えるときは CMake のキャッシュ変数を使う。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 \
      -DFLASH_FATFS_OFFSET=3145728 -DFLASH_FATFS_SIZE=1048576
```

どちらも 4096 バイト境界に揃え、フラッシュ全体に収まっていること。コンパイル時に
`_Static_assert` で検査する。**ファームウェアと重なっていないか**は起動時に
リンカシンボル `__flash_binary_end` と突き合わせて実行時に確認し、重なっていたら
マウントも書き込みも一切せずに `storage status` へ表示する。

### 7.2 PC から曲データをコピーする

```
> storage host
OK
```

PC にリムーバブルディスク `OPMVGM` が現れるので、`/VGM/` へ `.vgm` / `.vgz` を、
`/MDX/` へ `.mdx` を置く。終わったら **PC 側で取り出し（eject）する**。それだけで
Pico 側へ所有権が戻り、FatFs が自動でマウントし直される。

`/VGM` と `/MDX` はどちらも `storage format` が作る。それより前にフォーマットした
メディアには `/MDX` が無いので、その場合は PC 側でフォルダを作る（無いまま
`mdx list` を実行すると `ERR not found` になる）。

```
# storage : ejected by host
```

eject せずに `storage player` と打っても戻せるが、PC 側に書き込み途中のデータが
残っている可能性があるため警告が出る。

```
> storage player
# warn    : ホストが eject していません。ファイルが不完全な可能性があります
OK
```

**macOS では、一度取り出したあと同じ USB 接続のまま再びディスクとして現れさせることが
できない。** これは macOS 側の制約で、取り出した時点で USB マスストレージのドライバが
デバイスから切り離され、USB の再列挙まで戻ってこないため。ファームウェアが SCSI の
UNIT ATTENTION（メディアが入れ替わった）を返しても、リムーバブルメディアとして
申告していても変わらない。実際、`storage host` の LOEJ 処理を完全に無効にしても
マウントされないことを確認している。

対策は次のいずれか。

* **Pico の USB を挿し直す**（CDC も一度切れるが確実）
* コピーが 1 回で済むよう、必要なファイルをまとめて置いてから取り出す

USB を挿し直したくない場合、`storage host` と `storage player` の往復自体は
何度でもでき、`vgm list` や `vgm play` も正常に動く。**PC からもう一度書きたいときだけ**
挿し直しが要る。

新品の基板ではフラッシュ後半が全 `0xFF` なのでファイルシステムが無い。
**起動時に自動でフォーマットすることはしない**ので、最初に 1 度だけ実行する。

```
> storage format yes
# format  : 数十秒かかる。この間 I2S はアンダーランする
OK
```

### 7.3 書き込みの仕組みと制約

フラッシュの消去・書き込みのあいだは XIP が止まるため割り込みを落とす必要があり、
**4KiB ブロックあたり 40〜70ms メインループが止まる**。これはキャプチャの DMA リング
（65.5ms 分）と I2S の先行量（16.4ms 分）を超えるので、`storage host` のあいだは
PCM キャプチャと I2S 出力を無効にしている（[§3.13](#313-storageストレージ)）。

書き込みは 8 行 × 4KiB のライトバックキャッシュ（RAM 32KiB）を経由する。
PC からの書き込みはまずキャッシュに載り、次のいずれかでフラッシュへ落ちる。

* キャッシュに空きが無くなったとき（その場で最も古い 1 行を書き出す）
* PC からの書き込みが 250ms 途切れたとき（1 行ずつ流し切る）
* PC が eject したとき / `storage player` へ切り替えたとき（全部書き出す）

SCSI の `SYNCHRONIZE CACHE` では書き出さない。macOS はコピー中にこれを頻繁に
投げてくるため、素直に従うと 512 バイト書くたびに 4KiB ブロックを書き直すことになり、
書き込み回数が 1 桁増える。**取り外す前には必ず eject するか `storage player` に戻すこと。**

| 注意点 | 内容 |
| --- | --- |
| ファームウェアの再書き込み | OpenOCD の `program` も `picotool load` も ELF/UF2 の中身しか書かないので、**ファイルシステムは残る** |
| `picotool erase` / BOOTSEL の nuke UF2 | フラッシュ全体を消すので**ファイルシステムも消える** |
| 容量 | 2MiB。macOS が `.fseventsd` を作るぶん少し減る（Spotlight は `/.metadata_never_index` で抑止済み） |
| 同時アクセス | `PLAYER` では MSC がメディア非挿入を返し、`HOST` では FatFs をアンマウントするので、両者が同時にフラッシュを触ることはない |

### 7.4 マウントされないときの調べ方

`storage host` を実行しても PC にディスクが現れず、すぐに

```
# storage : ejected by host
```

が出ることがある。**まず疑うのは macOS の画面ロック。** macOS は画面がロックされている
あいだ、リムーバブルメディアの自動マウントを拒否してイジェクトする。ファイルシステムの
判別（`msdos_fskit`）まで成功したうえで `loginwindow` がマウント承認を拒否するので、
症状は「認識すらされない」ように見える。**画面のロックを解除してから `storage host` を
実行すれば、そのままマウントされる。**

macOS 側のログでは次のように出る。

```
$ log stream --style compact --level info
diskarbitrationd  probed disk, id = /dev/disk6, with msdos_fskit, success.
diskarbitrationd  dispatched callback, kind = disk mount approval, disk = /dev/disk6.
loginwindow       CopySLMountApprovalCallback | DiskArb - wholeDisk != nil, calling DADiskEject
diskarbitrationd  dispatched response, kind = disk mount approval, dissented, status = 0xF8DA0008
diskarbitrationd  ejected disk, id = /dev/disk6, success.
```

画面ロックが原因でないときは、ファーム側が受け取った SCSI を `storage trace` で見る。
`storage host` に入った時点で記録が始まり、直近 320 件が残る。

```
> storage trace
# msc     : 131 events (showing 131)
# msc 000 : T 01 01 00
# msc 001 : T 01 00 01
# msc 002 : P 01 00 00
# msc 003 : C 00 00 00
# msc 004 : W 01 00 00
# msc 005 : I 00 00 00
# msc 006 : R x115
# msc 121 : P 00 00 00
# msc 122 : S 00 00 01
OK
```

| 記号 | 意味 | 欄 |
| --- | --- | --- |
| `T` | TEST UNIT READY | メディア有無 / メディア交換の通知を返したか / 成功したか |
| `I` | INQUIRY | — |
| `C` | READ CAPACITY | — |
| `W` | 書き込み可否の問い合わせ（MODE SENSE の一部） | 書き込み可か |
| `P` | PREVENT/ALLOW MEDIUM REMOVAL | 取り出し禁止か |
| `S` | START STOP UNIT | 電源状態 / start / **load_eject** |
| `R` | READ10 | 連続する分は `R xN` とまとめる |
| `X` | 上記以外の SCSI | オペコード / 第 2 バイト / 転送長 |

読み方の要点:

- `T 01 01 00` → `T 01 00 01` と続いていれば、メディア交換の通知（UNIT ATTENTION）が
  正しく受理されている。ここで `T 00 ...` しか出ないならメディアを見せられていない
- `R` が数十件出ていれば、**ファイルシステムは読めている**。そのあとの
  `S 00 00 01`（load_eject）が PC 側からのイジェクト
- `R` が 1 件も無いまま `S 00 00 01` が来るなら、PC はファイルシステムを読む前に
  諦めている。ディスクリプタや容量の申告を疑う

## 8. VGM 再生

`/VGM/` に置いた VGM ファイルを、ファームウェア単体で（PC からシーケンスを流し込まずに）
再生する。操作は [§3.14](#314-vgmvgm-再生)。

### 8.1 対応範囲

**演奏対象は YM2151（VGM コマンド `0x54`）だけ。** 他の音源のコマンドは仕様上の
長さぶん読み飛ばす。デュアルチップのファイルは 2 個目（`0xA4`）を無視する。

| 項目 | 内容 |
| --- | --- |
| 対応バージョン | 1.00 以降（データ開始位置は v1.50 未満と 0 のとき 0x40 固定） |
| 処理するコマンド | `0x54`（YM2151 書き込み） / `0x61` `0x62` `0x63` `0x70`-`0x7F`（wait） / `0x80`-`0x8F`（DAC。書き込みは飛ばし wait だけ効かせる） / `0x66`（終端） / `0x67`（データブロックを飛ばす） |
| 読み飛ばすもの | 上記以外の既知コマンドを仕様上の長さぶん |
| 中断するもの | 未知のオペコード（`0x00`-`0x2F` / `0x60` / `0x65` / `0x69`-`0x6F` / `0x96`-`0x9F`）、ファイルの途中終端 |
| ループ | ヘッダのループオフセットが有効ならそこへ戻って無限に繰り返す。`vgm stop` するまで続く |
| 圧縮 | `.vgz`（gzip）を一時ファイルを作らずストリームのまま展開して再生する（[§8.5](#85-vgzgzipの再生)） |
| 非対応 | GD3 タグ（曲名・作者）の表示 |

不正なファイルを読んでも、無限ループや範囲外の読み出しは起きない。
どの経路も必ず 1 バイト以上消費し、シークは必ずファイルサイズと突き合わせる。

### 8.2 タイミング

VGM の wait は 44100Hz の絶対サンプル数で表される。予定時刻は `time_us_64()` と
突き合わせ、**発行済みの絶対サンプル数から毎回計算し直す**ので丸め誤差が累積しない。
φM は sys_clk の整数分周で、どちらも同じ水晶から出ているため長期的なずれも生じない。

1 回の処理は 500µs で打ち切り、遅れた分は次の周回で詰める。遅れの最大値は
`s` の `SEQ LAG` で見える（実測で 700µs 程度）。ループの継ぎ目でも時計を
リセットしないので、時間の不連続は生じない。

**ハードウェアタイマの割り込みは使わない。** レジスタ書き込み 1 回が 32µs
ブロックするため、キーオンが集中すると割り込み文脈を数 ms 占有して I2S への
供給を飢えさせる。ポーリングにすることで、書き込みの合間に必ず
キャプチャと I2S のサービスが挟まる。

### 8.3 φM と VGM のクロック

`vgm play` は、ヘッダ（オフセット `0x30`）が申告する YM2151 のクロックへ
**φM を自動で合わせる**。合わせ先は最寄りのプリセット（[§2](#2-クロック設定-φm)）で、
境界は 2 つの中点 3789772Hz。

| ファイルの申告値 | 選ばれるプリセット |
| --- | --- |
| 4000000 Hz | `4`（4.000000 MHz） |
| 3579545 Hz | `3.58`（3.579545 MHz） |
| 3546895 Hz（PAL）/ 3375000 Hz など | `3.58`（3.579545 MHz） |

キャプチャと I2S のサンプリングレート（φM/64）も一緒に変わる。切り替えは
`opm_clear()` より前に済むので、再生開始の時刻には影響しない。

寄せてもなお値が一致しないときだけ、残ったずれを警告する。

```
> vgm play SOMETHING.VGM
# vgm     : version 1.51  samples 705600  loop yes
# clock   : file 3546895 Hz / phiM 3579545 Hz (音程が低くなる)
OK
```

**ずれるのは音程と包絡線の速さだけで、テンポは正確なまま。** VGM の wait は
チップのクロックではなく 44100Hz の絶対サンプル数だからで、だから wait を
伸縮させるのは逆効果になる（テンポまで狂う）。

追従してほしくないときは `clock fixed`（[§3.16](#316-clockクロック切り替え)）。
このとき φM は動かず、食い違いは警告として出るだけになる。

**PCM キャプチャ中（`p 1`）に別クロックの VGM を再生しようとすると、再生自体が
`ERR wrong state` になる。** キャプチャ中のクロック切り替えを拒否しているため。
先に `p 0` を打つか、`clock fixed` にする。

v1.10 より前の VGM にはクロックのフィールドが無い。この場合は追従せず、
現在の φM のまま再生する。

### 8.4 既存機能との共存

VGM 再生中も YM3012 のキャプチャと I2S 出力はそのまま動く。

```
VGM ──> YM2151 ──> YM3012 ─┬─> USB CDC #1（p 1 で録音）
                            └─> I2S ──> PCM5102A
```

`p 1` を併用すれば、再生した VGM をそのまま WAV として録音できる。

### 8.5 `.vgz`（gzip）の再生

gzip 圧縮された VGM（`.vgz`）を、**一時ファイルを作らずに展開しながら**再生する。
ホスト側で圧縮して `/VGM/` へ置くだけでよい。

```bash
gzip -9 -c foo.vgm > foo.vgz
```

圧縮したままフラッシュに置けるので、**2MiB の領域に入る曲数が数倍になる**（YM2151 の
VGM は 3〜10 倍に縮む）。`vgm list` は `.vgm` と `.vgz` を区別せず名前順に並べ、
`vgm play` はどちらも受ける。

**圧縮されているかどうかは拡張子ではなくファイル先頭のマジック（`1F 8B`）で決める。**
中身が gzip の `.vgm` も、その逆も正しく扱える。

| 項目 | 内容 |
| --- | --- |
| 展開器 | miniz の `tinfl`（[§12](#12-ライセンス)） |
| 展開バッファ | 32KiB。DEFLATE の履歴窓を兼ねる |
| ループ | 継ぎ目に停止は出ない。非圧縮の `.vgm` と同じ（下記） |
| 追加で使う RAM | 約 86KiB。RP2350 の 512KiB に対して残りは約 330KiB |
| 追加で使う ROM | 約 10KiB |
| CRC32 | 検証しない（下記） |

**ループの継ぎ目は非圧縮と変わらない。** gzip は後方シークできないので、ループ先頭を
はじめて通過する瞬間に展開器の状態を丸ごと保存しておき、2 周目以降はそこへ戻す。
戻すのはメモリのコピーだけなので、頭出しのために展開し直すことはない。

保存できないまま通過してしまった場合の保険として、先頭から展開し直す経路がある。
こちらを通ると継ぎ目で音が数百 ms 途切れ、`s` の `gz reload` が増える
（[§3.11](#311-s統計)）。**通常の再生では 0 のまま。**

`0x67` のデータブロックのような前方への読み飛ばしは、gzip では飛ばせないので展開して
捨てるしかない。1KiB ずつに分けて 500µs の予算に乗せるため、読み飛ばしのあいだも
キャプチャと I2S への供給は止まらない。ここで生じた遅れは時計の張り直しが吸収する
（[§8.2](#82-タイミング)）。

**gzip トレーラの CRC32 は検証しない。** 読むのは ISIZE（展開後のサイズ）だけ。
ループ再生では終端に到達しないことが普通で、全部展開しないと計算できない CRC を
待っても壊れたファイルの検出は早まらないため。壊れていれば展開エラーか VGM 側の
未知オペコードとして必ず捕まる（[§3.14](#314-vgmvgm-再生)）。

同じ VGM を非圧縮と `.vgz` で 20 秒ずつ再生したときの実測（φM 4MHz、I2S 有効）。

| | 非圧縮 `.vgm` | `.vgz` |
| --- | --- | --- |
| 20 秒後の `VGM POS` | 889,033 samples | 889,633 samples |
| `CPU` | 32% | 32% |
| `SEQ LAG` の最大 | 621µs | 798µs |
| `I2S UNDERRUN` | 0 | 0 |

テンポは非圧縮と変わらない。VGM の wait は 44.1kHz の絶対サンプル数なので、
展開に時間がかかっても遅れとして現れるだけで、テンポには乗らない（[§8.2](#82-タイミング)）。

展開器ごと落としたいときは CMake のキャッシュ変数で無効にする。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 -DVGM_VGZ_ENABLED=0
```

無効にすると `.vgz` は `ERR bad file` になる（`# hint` 行で理由を出す）。実測サイズは
text 90,800 → 80,684 バイト / bss 180,448 → 94,012 バイト。

## 9. MDX 再生

`/MDX/` に置いた MDX ファイル（X68000 の音源ドライバ MXDRV 用のバイナリ MML）を、
ファームウェア単体で再生する。操作は [§3.15](#315-mdxmdx-再生)。

VGM が「レジスタ書き込みのタイムスタンプ付きダンプ」なのに対し、MDX は
**チャンネルごとに独立したシーケンサ**を持つ MML なので、音色・音量・ピッチ・LFO を
ファームウェア側で解釈してはじめて YM2151 のレジスタ値になる。つまり MXDRV 相当の
音源ドライバがファームウェアに入っている。

### 9.1 対応範囲

**FM 8ch と ADPCM 8ch。** MDX の MML コマンドは全部解釈する（音色・音量・パン・
ゲートタイム・レガート・リピート・ループ・ディチューン・ポルタメント・ソフト LFO
（音程 / 音量）・OPM のハードウェア LFO・同期・フェードアウト・キーオン遅延）。

**ADPCM パートはソフトウェアでデコードして鳴らす。** X68000 の ADPCM は MSM6258 と
いう別のチップで本機には載っていないので、PDX から読んだ波形をファームウェアが
デコードし、YM3012 から取り込んだ FM の PCM に足す。詳しくは
[§9.7](#97-adpcm-pcm8-の再生)。

チャンネル数はファイルが持つ 9（FM 8 + ADPCM 1）か 16（FM 8 + ADPCM 8。PCM8 拡張）。
16ch のファイルでは、`0xE8`（PCM8 モード宣言）が実行されるまで 9ch ぶんだけを回す。
ADPCM チャンネルは**シーケンサとしては FM と同じように回している**。同期コマンド
（`0xEF` / `0xEE`）が ADPCM 側から FM 側へ飛んでくる曲があり、止めてしまうと FM が
永久に待ち続けるため。

`.mdx` を gzip したものには対応しない。MDX 本体は数 KB〜数十 KB しかないので、
`.vgz` のような圧縮の利点が小さい。

### 9.2 タイミング

MDX のテンポは **OPM の Timer-B 由来**で、`@t`（`0xFF`）が書く 1 バイトがそのまま
Timer-B の値になる。1 clock の長さは

```
1024 × (256 - @t) φM サイクル
```

で、φM 4MHz なら `(256 - @t) × 256 µs`。既定の `@t 200` で 14336µs（約 70Hz）。

**この周期は φM サイクル数でちょうど整数になる**ので、φM から誤差なく µs へ直せる。
実装では 1/65536µs 単位の累算器に足し込んでいて、テンポを変えても `clock` で φM を
切り替えても、丸め誤差が溜まらない。

タイマ割り込みは使わず `time_us_64()` のポーリングで作る。OPM のレジスタ 0x12（Timer-B）
と 0x14（タイマ制御）には MXDRV と同じように書き込むが、`/IRQ` は誰も読まない。

1 tick で発行する書き込みが多いときは、**tick の途中で中断して次の周回へ持ち越す**。
音色の展開は 1 チャンネルあたり 25 レジスタ（約 800µs）で、これが複数チャンネルで
同じ tick に重なると 1 tick で 180 回以上になる。全チャンネルの処理が終わるまで
tick は進めないので、演奏が先へ流れることはない。

200ms を超えて遅れたときは早送りせず時計を張り直す（`s` の `reslip`）。

### 9.3 φM と テンポ

`mdx play` は φM を X68000 の **4MHz** へ寄せる（`clock auto` のとき。
[§2](#2-クロック設定-φm)）。`clock fixed` にしていれば従わない。

VGM と違い、**MDX は φM がずれるとテンポまでずれる。** VGM の wait は 44100Hz 固定の
絶対サンプル数なので φM とは無関係だが（[§8.3](#83-φm-と-vgm-のクロック)）、MDX の
1 clock は Timer-B 由来なので φM に反比例する。3.579545MHz で鳴らすと音程が約 10%
低くなるうえ、テンポも約 10% 遅くなる。

### 9.4 音程と音量の作り方

内部の音程は **1/64 半音**単位で持つ。MDX の音符 0-95 は `音符 × 64 + 5` が基準値で、
この `+5` は MXDRV の調律ぶん。ここへディチューン・ポルタメント・音程 LFO を足し込み、
最後に 4 倍して上位バイトを KC（`0x28+ch`）、下位バイトを KF（`0x30+ch`）にする。

KC の下位 4bit は 3 / 7 / 11 / 15 を飛ばすので、音符番号から KC への変換は 96 個の表で行う。
MDX の音符 0 は `o0 d+` で、これは OPM の KC 0 とちょうど一致する。

音量は `v0`-`v15`（16 段の表で TL へ）と `@v`（TL を直接指定）の 2 系統があり、
値の bit7 で区別する。TL を足すのは**キャリアのスロットだけ**で、どれがキャリアかは
音色の CON から決まる。フェードアウトと音量 LFO はこの TL に足し込む。

`t` コマンドがこの音程変換とテンポ換算を既知のベクタで検証する（[§3.12](#312-t自己テスト)）。

### 9.5 既存機能との共存

**VGM と MDX は同時に再生できない。** どちらかが再生中なら、もう一方の `play` は
`ERR wrong state` になる。

再生中は `w` / `r` / `c` と `storage host` を拒否する（理由は VGM と同じ。
[§8.4](#84-既存機能との共存)）。MDX 本体はファイルを丸ごと RAM に載せているので
アンマウントされても読めるが、**PDX は再生中もファイルを開いたまま読み続ける**
（[§9.7](#97-adpcm-pcm8-の再生)）。どちらにせよフラッシュの消去中は数十 ms 止まる
ので拒否する。

PCM キャプチャ（`p 1`）と I2S 出力は再生中も動く。`mdx play` が φM を切り替えようと
したときにキャプチャ中だと `ERR wrong state` になる。

### 9.6 MXDRV との相違点

レジスタへの書き込み内容と順序は MXDRV 2.06+17 に合わせてある。違うのは 1 点だけ。

**BUSY 待ちをしない。** MXDRV は書き込みのたびに OPM のステータスレジスタを読んで
bit7 が下りるのを待つが、本機はデータバスが出力専用（`/RD` は H 固定）でステータスを
読めない。代わりに固定ウェイト（アドレス後 5µs / データ後 25µs、合計約 32µs）を使う。
X68000 実機より遅い方向なので、詰まるとすれば書き込みが間に合わない側に出る。
`s` の `MDX TEMPO` の `reslip` と I2S のアンダーランで検出できる。

ADPCM については、X68000 では PCM8（江藤啓氏の ADPCM 多重再生ドライバ）が
MSM6258 を叩く。本機はその PCM8 に相当する処理をソフトウェアで持っている
（[§9.7](#97-adpcm-pcm8-の再生)）。

### 9.7 ADPCM (PCM8) の再生

MDX ファイルのヘッダには PDX（ADPCM の波形集）の名前が入っている。
`mdx play` はその名前に `.PDX` を付けた **`/MDX/<名前>.PDX`** を開く。大小文字は
区別しないので、`THEXDER` という名前で `thexder.pdx` も見つかる。
見つからなくてもエラーにはならず、FM パートだけがそのまま鳴る。

```
> mdx play BOS14.MDX
# mdx     : BOS14.MDX
# title   : Ｂlast Ｐower ！ ～ from BOSCONIAN-X68
# ch      : 9  voices 8
# pdx     : bos
# adpcm   : /MDX/bos.PDX
OK
```

PDX は数百 KB あるので RAM には載せず、**再生しながらフラッシュから読む**。
`storage host` が再生中に拒否されるのはこのためでもある（[§9.5](#95-既存機能との共存)）。
`mdx stop` と曲の終わりでファイルは閉じる。

#### 鳴らし方

X68000 の ADPCM は 5 通りの再生周波数を持ち、MDX の `@ED` がそれを選ぶ。
本機の出力レートは φM/64 で、ADPCM のレートも φM の分周なので、**比は必ず整数**になる。

| `@ED` | 形式 | レート | 出力フレーム/サンプル |
| --- | --- | --- | --- |
| 0 | ADPCM | 3.90625 kHz | 16 |
| 1 | ADPCM | 5.20833 kHz | 12 |
| 2 | ADPCM | 7.8125 kHz | 8 |
| 3 | ADPCM | 10.41667 kHz | 6 |
| 4 | ADPCM | 15.625 kHz | 4 |
| 5 | 16bit PCM | 15.625 kHz | 4 |
| 6 | 8bit PCM | 15.625 kHz | 4 |

整数比なので**補間はしない**。1 サンプルをそのまま所定の回数だけ繰り返す。これは
MSM6258 の出力（階段波）そのもので、実機に一番近い。`mdx play` は φM を X68000 と
同じ 4MHz へ寄せるので、実レートも実機と一致する。

音量は PCM8 の仕様どおり **1 step = 2dB で 8 が原音**（-16dB〜+14dB）。MML の
`v` / `@v` とフェードアウトから PCM8 の 0-15 を作る。定位は PCM8 の仕様どおり
**全チャンネル共通**で、0 以外で最後に指定された値が有効になる。

**音量の調整つまみは無い。** ADPCM のフルスケールを FM のフルスケールと同じ重みで
足す、実機に忠実な固定のレベルにしてある。両方が大きいときに足して 16bit を超えた
回数は `s` の `MDX PCM` の `CLIP` に出る（実機のアナログミックスでも歪む領域）。

#### 出力先

ADPCM は **I2S 出力にも PCM キャプチャ（`p 1`）にも同じように乗る。** YM3012 から
取り込んだ値を PCM へ変換した直後の 1 箇所で足しているので、どちらの経路にも同じ音が
流れる。`mdx pcm off` にすると FM だけになるので、聴き比べや解析に使える
（ADPCM 側の解析をしたくないときにも使う）。

#### 対応していないこと

- PCM8 のチェイン出力（アレイチェイン / リンクチェイン）。MDX からは使われない。
- PCM8 の単音再生モードと、常駐判定・占有などの管理用ファンクション。
- `0xE7 0x02`（MDX から PCM8 へのコマンド）のうち、本機に対応する設定が無いもの
  （多重 / 単音モードの切り替えなど）。読み飛ばすだけで演奏は続く。

## 10. ホスト側ツール

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

## 11. 将来の拡張（本仕様の範囲外）

- **PCM8 のチェイン出力（アレイチェイン / リンクチェイン）。** MDX からは使われない
  ので後回しにしている（[§9.7](#97-adpcm-pcm8-の再生)）
- バイナリストリーミングモード（`w` の 1 行あたりのオーバーヘッド削減）
- VGM の GD3 タグ（曲名・作者）の表示
- GPIO のボタン / ロータリーエンコーダ / 表示器による操作（現在は CDC #0 のコマンドのみ）
- `/RD` を使ったステータス（BUSY）ポーリングによる待ち時間の最適化。配線と
  データバスの向き切り替え（`opm_bus_set_dir()`）は用意してあるが、読み出し自体は未実装
- `/IRQ` の割り込み処理。現在はレベルを参照できるだけ（`s` の `IRQ`）
- バス書き込みの PIO 化と FIFO による非同期キューイング
- 2 個目の OPM / 他の Yamaha 音源チップ (OPN 系) への対応

## 12. ライセンス

MIT License。詳細は [LICENSE](LICENSE) を参照。

ただし `pico_sdk_import.cmake` は Raspberry Pi (Trading) Ltd. 由来のファイルで、
BSD-3-Clause が適用される（ファイル先頭の表示のとおり）。

`external/` に置いた外部ソースはそれぞれの条件による（[external/README.md](external/README.md)）。

MDX の解釈（[§9](#9-mdx-再生)）は X68000 の音源ドライバ **MXDRV 2.06+17**
（(c)1988-92 milk., K.MAEKAWA, Missy.M, Yatsube）の仕様に準拠している。
MXDRV のソースは本リポジトリには含まれておらず、`mdx.c` は独自に書き起こしたもの。

ADPCM パート（[§9.7](#97-adpcm-pcm8-の再生)）は **PCM8 version 0.48**
（(c) 江藤啓 1991,92）の技術資料に準拠している。PCM8 のソースやバイナリは本リポジトリ
には含まれておらず、`pcm8.c` は独自に書き起こしたもの。

| 置き場所 | ライセンス |
| --- | --- |
| `external/fatfs/` — FatFs R0.16（ChaN） | 1 条項の BSD 風。[external/fatfs/LICENSE.txt](external/fatfs/LICENSE.txt) |
| `external/miniz/` — miniz 3.1.2 | MIT（本プロジェクトと同一）。[external/miniz/LICENSE](external/miniz/LICENSE) |
