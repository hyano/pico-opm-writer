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

Pico 2 は OPM のバスへ双方向に接続する。**データバスを駆動するのは通常 Pico 側だけ**で、
読み出し（[`r`](#321-r読み出し)）の実行中だけ Pico 側を入力へ倒して OPM に駆動させる。
これとは別に、YM3012 (DAC) のシリアル出力を Pico 側へ取り込む（[§4](#4-pcm-出力)）。

| Pico 2 GPIO | 物理ピン | 接続先 | 方向 | 備考 |
| --- | --- | --- | --- | --- |
| GP0  | 1  | -    | -   | 将来のデバッグ UART TX 用に予約 |
| GP1  | 2  | -    | -   | 将来のデバッグ UART RX 用に予約 |
| GP2  | 4  | D0   | I/O | データバス。通常は出力、読み出し中だけ入力 |
| GP3  | 5  | D1   | I/O | |
| GP4  | 6  | D2   | I/O | |
| GP5  | 7  | D3   | I/O | |
| GP6  | 9  | D4   | I/O | |
| GP7  | 10 | D5   | I/O | |
| GP8  | 11 | D6   | I/O | |
| GP9  | 12 | D7   | I/O | D0-D7 は連続した GPIO であること（マスクでまとめて読み書きするため） |
| GP10 | 14 | A0   | OUT | L=アドレスラッチ / H=データ書き込み |
| GP11 | 15 | /CS  | OUT | チップセレクト（負論理） |
| GP12 | 16 | /WR  | OUT | 書き込みストローブ（負論理） |
| GP13 | 17 | /RD  | OUT | 読み出しストローブ（負論理）。[`r`](#321-r読み出し) の実行中だけ L |
| GP14 | 19 | /IC  | OUT | ハードウェアリセット（負論理） |
| GP15 | 20 | φM   | OUT | マスタークロック、PIO で生成 |
| GP16 | 21 | /IRQ | IN  | 割り込み要求（負論理）。プルアップ、レベル参照のみ |
| GP17 | 22 | SO   | IN  | YM3012 シリアルデータ。PIO の in_base |
| GP18 | 24 | φ1   | IN  | YM3012 ビットクロック（φM/2） |
| GP19 | 25 | SH1  | IN  | CH2 のサンプルホールド。フレーム同期に使う |
| GP20 | 26 | SH2  | IN  | CH1 のサンプルホールド。現在は未使用 |
| GP21 | 27 | SW1  | IN  | ボタン 1（[§3.20](#320-ボタンgp21--gp22)）。内部プルアップ、押下で GND |
| GP22 | 29 | SW2  | IN  | ボタン 2（同上） |
| GP25 | -  | -    | OUT | 基板上 LED（[§3.9](#39-led)）。`PICO_DEFAULT_LED_PIN` |
| GP26 | 31 | BCK  | OUT | I2S ビットクロック（[§5](#5-i2s-出力)） |
| GP27 | 32 | LRCK | OUT | I2S ワードセレクト。BCK の次の GPIO であること |
| GP28 | 34 | DIN  | OUT | I2S データ |
| RUN  | 30 | SW3  | IN  | Pico 2 のリセット端子。押下で GND に落としてリセット。**ファームウェアからは見えない**（[§3.20](#320-ボタンgp21--gp22)） |
| GND  | 3, 8, 13, 18 … | GND | - | OPM と共通グラウンドを取ること |

GP23 / GP24 は Pico 2 の内部用途（`PICO_SMPS_MODE_PIN` / `PICO_VBUS_PIN`）なので使わない。
I2S の MCLK は使わないので、DAC へ出すのは GP26-GP28 の 3 本だけ。

SO / φ1 / SH1 / SH2 は **GP17 から連続していること**。キャプチャ用の PIO がこの 4 本を
in_base からのオフセットで参照する（[docs §4.2](docs/pico-opm-writer.md#42-pio-によるビット取り込み)）。

ピン番号の定義は [opm.h](src/opm.h) の `OPM_PIN_*`、[ym3012.h](src/ym3012.h) の `YM3012_PIN_*`、
[button.h](src/button.h) の `BUTTON_PIN_*` にまとまっている。

### 1.2 GPIO を割り当てない OPM 端子

| OPM 端子 | 処理 | 理由 |
| --- | --- | --- |
| CT1 / CT2 | 未接続で可 | 汎用出力端子。本ファームウェアは使わない |

### 1.3 配線上の注意

**Pico 2 と OPM は全線を直結する。** レベル変換器も分圧も挟まない。
[board/](board/) の基板もこの前提で起こしてある。

- **入力側（Pico → OPM）**: OPM は 5V デバイス、Pico 2 の GPIO は 3.3V。RP2350 の 3.3V 出力を
  OPM が H / L として判別できることは実機で確認済みで、[test/](test/) の実測データはすべて
  この直結構成で取れている。ただし OPM の入力 H レベル閾値には個体差・ロット差があり、
  他の個体でも同じように動く保証は無い（未確認）。動作しない場合はバッファ / レベルシフタ
  （74HCT244 等）を φM とバス側に挿入する。
- **出力側（OPM → Pico）**: SO / φ1 / SH1 / SH2 / /IRQ は 5V デバイスである OPM の**出力**で、
  H レベルは 3.3V を超える。**RP2350 は A4 ステッピングから公式に 5V トレラント**なので
  直結でよい（[Raspberry Pi の告知](https://www.raspberrypi.com/news/rp2350-a4-rp2354-and-a-new-hacking-challenge/)、
  2025-07-29）。成立の条件は次の 3 つで、本構成はいずれも満たしている。

  | 条件 | 本構成での成立 |
  | --- | --- |
  | チップが **A4 以降**であること | A2 では成立しない（下記の確認方法） |
  | 対象は **GPIO 0-25 のみ**。GP26-GP29 は ADC 兼用で対象外 | 5V を受けるのは GP2-GP9 (D0-D7) / GP16 (/IRQ) / GP17-GP20 (SO・φ1・SH1・SH2) だけで、すべて範囲内 |
  | 5V が掛かっている間 **IOVDD が 3.3V で給電されている**こと。無給電のまま 5V を掛けるとパッドが壊れる | OPM の 5V は Pico の VBUS から取るので、5V があるときは必ず Pico の 3.3V も生きている |

  絶対最大定格は任意のピンで 5.5V。/IRQ は [opm.c](src/opm.c) が内部プルアップを張るので、
  外付け抵抗は要らない。

  **GP26-GP28（I2S の BCK / LRCK / DIN）に 5V デバイスを繋いではいけない。**
  この 3 本は 5V トレラントの対象外。接続先の PCM5102A は 3.3V ロジックで、
  しかも Pico からの出力専用なので現状は問題にならない（[§5](#5-i2s-出力)）。

  チップの版数は SDK の `rp2350_rom_version()` で読める（2 = A2 / 3 = A3 / 4 = A4）。
  A2 の個体を使う場合は SO / φ1 / SH1 / SH2 / /IRQ（読み出しを使うなら D0-D7 も）に
  レベル変換器か分圧を挟むこと。
  φ1 は 2MHz で動くので、分圧で済ませる場合は時定数に注意する。
- **バスの衝突防止**: D0-D7 は双方向なので、OPM と Pico が同時に駆動すると衝突する。
  /RD を H に保っていれば OPM は D0-D7 を駆動しないため、ファームウェアは
  **読み出し（[`r`](#321-r読み出し)）の実行中以外は /RD を H に固定**している。
  読み出しでは、OPM に駆動させる前に Pico 側を入力へ倒し、/RD を H に戻して OPM が
  バスを手放してから出力へ戻す。この間 D0-D7 は OPM の 5V 出力を受けるので、
  上の表の条件（A4 以降・GPIO 0-25・IOVDD 給電中）がそのまま効く。
  **A2 の個体で読み出しを使う場合は D0-D7 にもレベル変換器か分圧が要る。**
  **ただしこれが効くのは GPIO を初期化した後だけ。** RP2350 のパッドはリセット直後
  入力 + プルダウン（`PADS_BANK0_GPIO0_RESET` の `PDE` = 1）なので、電源投入から
  `opm_init()` が /CS と /RD を H にするまでの間は OPM から見て /CS = L / /RD = L となり、
  **OPM が D0-D7 を駆動する窓がある**。この窓では Pico 側がまだ入力なので駆動は衝突せず、
  D0-D7 に乗る 5V も上の表の条件を満たす。窓そのものを塞ぎたければ /CS を 5V へ
  10kΩ 程度でプルアップする（このとき GP11 も 5V を受けるが、GPIO 0-25 なので同じく
  条件の内側）。
- 電源には十分なデカップリング（OPM の VCC-GND 間に 0.1µF + 10µF 程度）を入れる。
- **ボタンは 3 個ともスイッチ 1 個だけでよい。** SW1 / SW2 は GP21 / GP22 と GND の間、
  SW3 は RUN と GND の間に入れる。**外付けのプルアップ抵抗もコンデンサも要らない**
  （GP21 / GP22 は RP2350 の内部プルアップを張って負論理で読み、チャタリングは
  ファームウェア側で時間ベースに除去する。RUN は Pico 2 側で既にプルアップされている）。
  SW1 / SW2 を使わないなら GP21 / GP22 は開放でよい。内部プルアップで「離されている」と
  読めるので、ボタンを実装していない基板でも既定のファームウェアがそのまま動く。

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
- **PCM を流している間は切り替えを拒否する。** サンプリングレートが
  ストリームの途中で変わると、ホスト側で出来上がる WAV の時間軸が黙って狂うため。
  `p 2` の待機中はまだ流していないので通る（[§3.16](#316-clockクロック切り替え)）

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

各コマンドは必ず 1 行の応答で終わる。**例外は空行とコメント行だけ**で、これらは
何も返さない（[§3.2](#32-行フォーマット)）。

| 応答 | 意味 |
| --- | --- |
| `OK` | 正常終了 |
| `ERR <理由>` | エラー |

情報を返すコマンドは、`#` で始まる情報行を 0 行以上出力したあと最後に `OK` を返す。
ホスト側は「`#` 始まりは情報、それ以外の 1 行が終端」と扱えばよい。

情報行は 4 種類ある。いずれも `# ` で始まるので、区別せず読み飛ばしても構わない。

| 情報行 | 意味 |
| --- | --- |
| `# <tag>   : ...` | 通常の情報。`<tag>` は主題（`vgm` / `adpcm` / `capture` など）を表す |
| `# hint    : ...` | **直後の** `ERR` の理由と対処。応答より先に出る。**`ERR wrong state` には必ず付く** |
| `# warn    : ...` | 処理は続いたが注意が要ること |
| `# ERR ...` | コマンドの応答ではない非同期の通知（[§3.18](#318-非同期通知)） |

**ファームウェアの応答はすべて英語。** 例外は `mdx play` / `mdx status` の `# title` 行
だけで、これは MDX ファイル中の Shift_JIS をそのまま流している（[§3.15](#315-mdxmdx-再生)）。

エラー理由の一覧:

| 応答 | 発生条件 |
| --- | --- |
| `ERR unknown command` | 未知のコマンド。1 文字のコマンドか `clock` / `storage` / `vgm` / `mdx` / `autoplay` / `reset` / `help` のいずれでもない場合と、それらのサブコマンドが未知の場合（`clock 5` のような未知のプリセット名も含む） |
| `ERR bad argument` | 引数が 16 進数 / 10 進数として解釈できない、ファイル名が不正、または語の引数が想定外（`mdx pcm maybe` / `storage format no`） |
| `ERR wrong arity` | 引数の個数が合わない |
| `ERR out of range` | 引数が許容範囲外 |
| `ERR too long` | 行が長すぎる |
| `ERR wrong state` | いまの状態では実行できない。**直前に必ず理由を示す `# hint` 行が出る。** どのコマンドがどの状態で拒否されるかは [§3.19](#319-状態による拒否の一覧) にまとめてある |
| `ERR no filesystem` | ストレージが未フォーマット、または領域がファームウェアと重なっている |
| `ERR not found` | 指定したファイルが無い、`/VGM` / `/MDX` が無い、または `autoplay list` を打ったときにプレイリストがまだ作られていない（[§3.17](#317-autoplay自動再生)） |
| `ERR bad file` | VGM / MDX として読めない（マジック不正 / ヘッダが壊れている / gzip ストリームが壊れている / MDX が 64KiB を超える） |
| `ERR io error` | フラッシュまたはファイルシステムの入出力に失敗した |
| `ERR not connected` | CDC #1 が開かれていない状態で `p 1`（[§3.10](#310-ppcm-出力)） |
| `ERR drain timeout` | `p 0` のドレインが 2 秒で終わらなかった（[§3.10](#310-ppcm-出力)） |
| `ERR self test failed` | `t` の自己テストのどれかが失敗した（[§3.12](#312-t自己テスト)） |
| `ERR unsupported` | この構成に機能が無い。`clock` の切り替え先の sys_clk をこのチップで生成できない（[§3.16](#316-clockクロック切り替え)）か、ビルド時に無効化されている（`MDX_ENABLED=0` / `PCM8_ENABLED=0`。[§9.8](#98-無効化)、`AUTOPLAY_ENABLED=0`。[§3.17](#317-autoplay自動再生)） |

### 3.4 コマンド一覧

| コマンド | 書式 | 説明 |
| --- | --- | --- |
| `w` | `w <addr> <data> [<addr> <data> ...]` | レジスタ書き込み。引数は 16 進。addr/data のペアを 1 行に複数並べて連続書き込みできる。**途中でエラーになった場合、そこまでの書き込みは実行済み**のまま `ERR` を返す |
| `r` | `r 0` / `r 1` | A0=0 / A0=1 で 1 バイト読み出し（[§3.21](#321-r読み出し)） |
| `reset` | `reset` | /IC によるハードウェアリセット。I2S のアンダーランを 1 回伴う（[§5.3](#53-アンダーラン)） |
| `c` | `c` | ソフトウェアによる全レジスタクリア（[§3.5](#35-cクリアが書き込む内容)） |
| `d` | `d <ms>` | 指定ミリ秒待機。10 進、`0`-`60000`。待っている間も PCM の送出は続く |
| `p` | `p` / `p 1` / `p 2` / `p 0` | PCM 出力の状態表示 / 開始 / 演奏に連動して開始 / 停止（[§3.10](#310-ppcm-出力)） |
| `s` | `s` / `s 0` | 統計の表示 / リセット（[§3.11](#311-s統計)） |
| `t` | `t` | 自己テスト（[§3.12](#312-t自己テスト)） |
| `i` | `i` | 情報表示（[§3.6](#36-i情報表示の出力例)） |
| `h` | `h` / `?` / `help` | コマンド一覧を表示 |
| `clock` | `clock` / `clock status` / `clock 4` / `clock 3.58` / `clock auto` / `clock fixed` | φM の表示と切り替え（[§3.16](#316-clockクロック切り替え)） |
| `storage` | `storage` / `storage status` / `storage host` / `storage player` / `storage format [force] yes` / `storage trace` | ストレージの状態表示とモード切り替え（[§3.13](#313-storageストレージ)） |
| `vgm` | `vgm` / `vgm status` / `vgm list` / `vgm play <path>` / `vgm stop` / `vgm loop [<n>]` / `vgm fade [<ms>]` | VGM の状態表示・一覧・再生、演奏の終わり方（[§3.14](#314-vgmvgm-再生)） |
| `mdx` | `mdx` / `mdx status` / `mdx list` / `mdx play <path>` / `mdx stop` / `mdx loop [<n>]` / `mdx fade [<ms>]` / `mdx pcm [on\|off]` | MDX の状態表示・一覧・再生、演奏の終わり方、ADPCM ミキシングの表示と切り替え（[§3.15](#315-mdxmdx-再生)） |
| `autoplay` | `autoplay` / `autoplay status` / `autoplay list` / `autoplay start` / `autoplay stop` / `autoplay next` / `autoplay prev` / `autoplay mode <list\|random>` / `autoplay loop <n>` / `autoplay fade <ms>` / `autoplay gap <ms>` / `autoplay source <vgm\|mdx\|both>` | VGM と MDX の自動連続再生（[§3.17](#317-autoplay自動再生)） |

コマンド体系の規則:

- **引数を省くと状態表示になる。** `p` / `clock` / `storage` / `vgm` / `mdx` / `mdx pcm` /
  `vgm loop` / `vgm fade` / `mdx loop` / `mdx fade` / `autoplay` は引数なしで現在の状態を
  返す。`clock status` / `storage status` /
  `vgm status` / `mdx status` / `autoplay status` は引数なしの形と同じ（打ちやすい方を
  使えばよい）。
- **状態を変えるコマンドは、変えたあとの状態を返す。** `p 1` / `p 2` / `p 0` / `storage host` /
  `storage player` / `clock 4` / `mdx pcm on` は変更後の状態を出してから `OK` を返す。
- **「止める」「切り替える」は冪等。** `vgm stop` / `mdx stop` / `autoplay stop` / `p 0` /
  `storage host` / `storage player` / `clock 4` は、既にその状態でも `OK` を返す。スクリプトから
  「いま何が動いているか分からないがとにかく止めたい」を書けるようにするため。
- **再生の開始は切り替えでもある。** `vgm play` / `mdx play` は何かが鳴っていても
  受け付け、走っている方（VGM / MDX のどちらでも）を止めてから始める。曲を変えるのに
  `stop` を先に打つ必要はない。
- **引数の基数は `w` だけ 16 進、他は 10 進。** `w` の 16 進は桁数自由で、値が `0xff`
  以下なら受理する（`w f 1` も `w 0020 00c7` も通る）。`0xff` を超えると
  `ERR out of range`。`d <ms>`（`0`-`60000`）と `p 1` / `p 2` / `p 0` / `s 0` は 10 進。
- **引数の語彙は、1 文字コマンドは数値、複数文字コマンドは語。** 1 文字コマンドは
  打鍵の短さを優先して `r 0` / `r 1` / `p 1` / `p 2` / `p 0` / `s 0` / `d 500` とし、複数文字コマンドは
  `on` / `off` / `auto` / `fixed` / `host` / `player` のように語で書く。
  `p on` のような書き方は受け付けない。
- **大小は区別しない。** 1 文字のコマンドも `clock` / `storage` / `vgm` / `mdx` /
  `autoplay` / `reset` / `help` とそのサブコマンドも同様。
- **自動再生とストレージの操作は、基板上のボタンからも行える。** `autoplay start` /
  `autoplay mode` / `autoplay next` / `autoplay prev` / `storage host` に相当する操作を
  SW1 / SW2 の押し方で選べる（[§3.20](#320-ボタンgp21--gp22)）。ボタン起点の操作は
  `#` で始まる情報行しか出さないので、「1 コマンド 1 応答」（[§3.3](#33-応答)）は崩れない。
- **`vgm play` と `mdx play` のファイル名だけは行の残り全部を 1 引数として受ける**ので、
  空白を含む名前もそのまま書ける（`vgm play BAD NAME.VGM`）。サブフォルダは `/` で
  区切って書く（`vgm play KONAMI/GRADIUS.VGM`）。

1 レジスタ書き込みには約 32µs かかる（最大 3 万回/秒）。内訳は
[docs §3.1](docs/pico-opm-writer.md#31-タイミング定数)。

### 3.5 `c`（クリア）が書き込む内容

/IC リセットに近い状態をソフトウェアで作る。実行順:

1. `0xE0`-`0xFF`（全 32 スロットの D1L/RR）に `0x0F`（D1L = 0 / RR = 15、最速リリース）。
   KEY OFF より前に書くので、直前の音色の RR に関わらず即座に減衰する
2. `0x08` に `0x00`-`0x07` を書き、全 8 チャンネルを KEY OFF
3. `0x60`-`0x7F`（全 32 スロットの TL）に `0x7F`（最小音量）
4. `0x0F` に `0x00`（ノイズ off）
5. `0x14` に `0x00`（タイマ / IRQ 停止）
6. `0x01` に `0x00`（LFO リセット解除）
7. `0x18` に `0x00`（LFO 周波数）
8. `0x19` に **`0x80` → `0x00` の順で 2 回**書き込む。`0x19` は bit7 で書き込み先が切り替わる
   レジスタで、`0x80` が PMD = 0、`0x00` が AMD = 0 の設定になる（PMD 側を消し忘れないため
   両方を書く）
9. `0x1B` に `0x00`（LFO 波形 / CT1・CT2）
10. `0x20`-`0x27` に `0x00`（RL / FB / CONNECT）

### 3.6 `i`（情報表示）の出力例

```
# pico-opm-writer 0.3.0
# phiM    : 4000000 Hz (clkdiv 18 + 0/256)
# sys_clk : 144000000 Hz
# preset  : 4  (vgm auto)
# pins    : D0-D7=GP2-GP9 A0=GP10 /CS=GP11 /WR=GP12 /RD=GP13 /IC=GP14
# pins    : phiM=GP15 /IRQ=GP16
# timing  : t_wr=1us t_addr=5us t_data=25us t_rd=1us
# ym3012  : SO=GP17 phi1=GP18 SH1=GP19 SH2=GP20
# pins    : SW1=GP21 SW2=GP22 (pull-up, active low)
# button  : long 1000 ms  boot none
# capture : ring 16384 bytes (4096 frames) rate 62500 Hz
# i2s     : BCK=GP26 LRCK=GP27 DIN=GP28 (clkdiv 36 + 0/256)
# i2s     : 32fs bck 2000000 Hz rate 62500 Hz latency 1024 frames (16384 us)
# piotest : SKIP (disabled)
# storage : flash 0x040000 + 3836 KiB  cluster 4096 B  sector 512 B
# vgm     : dir /VGM  rate 44100 Hz  budget 500 us
# mdx     : dir /MDX  max 64 KiB  budget 500 us
# adpcm   : enabled  8 ch  pdx stream 1024 bytes/ch
OK
```

バージョンに続く 3 行は `clock`（[§3.16](#316-clockクロック切り替え)）の先頭 3 行と
同じ内容・同じ並び。
`I2S_ENABLED=0` でビルドした場合は `# i2s` の 2 行が `# i2s     : disabled` の 1 行に
なる（[§5.4](#54-無効化)）。同じく `BUTTON_ENABLED=0` では `# pins : SW1=...` と
`# button` の 2 行が `# button  : disabled` の 1 行になる（[§3.20](#320-ボタンgp21--gp22)）。

`# button` の `boot` は**起動時にボタンで選んだ動作モードとその結果**
（[§3.20](#320-ボタンgp21--gp22)）。押されていなければ `none`。起動直後の出力は
ホストが CDC を開く前なので捨てられるため、後から USB を挿しても分かるようここに残す。

```
# button  : long 1000 ms  boot autoplay list (ok)
# button  : long 1000 ms  boot autoplay random (no filesystem)
# button  : long 1000 ms  boot storage host (ok)
```

### 3.7 起動バナー

USB CDC の接続を検出した時点で `i` と同じ内容を出力する（末尾の `OK` を含む）。

### 3.8 使用例

```
> i
# pico-opm-writer 0.3.0
...
OK
> reset
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
| `storage host` 中 | 200ms ON / 800ms OFF | ファイルシステムを PC へ渡している（[§3.13](#313-storageストレージ)） |
| 起動時のモード選択 | 100ms 点滅 ×n → 休み | ボタンを押しながら起動した。**n が選ばれたモード**（1 = `autoplay list` / 2 = `autoplay random` / 3 = `storage host`）。離すまで続く（[§3.20](#320-ボタンgp21--gp22)） |
| コマンド受信 | 100ms ON → 100ms OFF → 100ms ON | 1 回だけ差し込む。終わったら元のパターンの先頭へ戻る |
| ボタンの長押し成立 | 100ms OFF → 500ms ON → 100ms OFF | 1 回だけ差し込む。しきい値に達した瞬間に出る（[§3.20](#320-ボタンgp21--gp22)） |
| DMA overrun | 100ms ON/OFF ×3 → 1 秒 OFF | [§4.1](#41-取りこぼしたとき-dma-overrun)。エラー表示はこれだけ |

キャプチャ中の 500ms 点滅とコマンド受信の 100ms 二重点滅は速さで区別できる。
`storage host` 中の 200/800 は、点いている時間の短さでキャプチャ中の 500/500 と分かれる。
一時表示の 2 つは、コマンド受信が短い 2 連（計 300ms）、ボタンの長押し成立が
1 発の長い点灯（計 700ms）で区別できる。**長押しの合図が OFF から始まる**のは、
一時表示が基本パターンを完全に置き換えるため。待機（常時点灯）の上に点灯だけの
パターンを重ねても何も変化しない。

起動時のモード選択が 3 回点滅する場合（`storage host`）は DMA overrun と点滅の回数が
同じだが、**DMA overrun は `p 1` の後にしか起き得ない**ので起動待ちと同時には現れない。
休みの長さも違う。

**LED をエラー表示にするのは DMA overrun のときだけ。** `p` の状態エラー・CDC #1 の切断・
USB 未接続・一時的な USB 送信不可は、いずれもエラー表示にしない。

### 3.10 `p`（PCM 出力）

CDC #1 への PCM 送信を制御する。**取り込み側の PIO と DMA は起動時から常時動いていて、
このコマンドでは開始も停止もしない**（[§4](#4-pcm-出力)）。

| 書式 | 動作 |
| --- | --- |
| `p` | 現在の状態を表示する（下記） |
| `p 1` | このコマンドを処理した時点以降に DMA が書いたデータから送信を始める。それより前のデータは送らない |
| `p 2` | **次の `vgm play` / `mdx play` から送信を始め、その曲が終わったら自分で停止する**（下記） |
| `p 0` | このコマンドを処理した時点までに DMA が取り込んだデータを**最後の 1 フレームまで送り切ってから**停止する |

`p 1` / `p 2` / `p 0` は変更後の状態を 1 行出してから `OK` を返す。

```
> p
# capture : IDLE
# auto    : off
# cdc1    : connected
# rate    : 62500 Hz
OK

> p 1
# capture : CAPTURING
OK
```

`# capture` は `s` の `STATE` と同じ（`IDLE` / `WAITING` / `CAPTURING` / `DRAINING` /
`ERROR`。[§3.11](#311-s統計)）。`# auto` は `p 2` で始めたか、`# cdc1` は PCM 側の
ポートをホストが開いているか、`# rate` はサンプリングレート φM/64。

#### `p 1` と `p 2` の違い

**違うのは録り始めと録り終わりだけ。** 送り先（CDC #1）も PCM の形式（[§4](#4-pcm-出力)）も
ドレインの手順も overrun の条件も `storage host` の拒否も同じ。

| | `p 1` | `p 2` |
| --- | --- | --- |
| コマンド直後 | 即座に流し始める | **何も流さない**（`WAITING`）。待っている間の無音は WAV に入らない |
| 録り始め | `p 1` の瞬間 | 次に成功した `play` の瞬間 |
| 既に鳴っているとき | その時点から録る | 何もしない。**次の** `play` を待つ |
| 録り終わり | `p 0` まで無制限 | その曲が終わって**余韻も消えたとき**（[§3.22](#322-曲の終わり方)） |
| 終了の通知 | 無し（`p 0` の応答が終端） | `# capture : done <n> frames` |
| φM の切り替え | 拒否 | **待機中は通る。** 録り始めれば `p 1` と同じ扱い（[§3.16](#316-clockクロック切り替え)） |

φM だけ扱いが違うのは、待機中はまだ 1 フレームも流していないためで、これが
別クロックの VGM を `p 2` で録れる理由になっている。

```
> p 2
# capture : WAITING
OK
> mdx play SORCER/OP.MDX
# mdx     : SORCER/OP.MDX
OK
（曲が鳴り終わるまで CDC #1 に PCM が流れ続ける）
# mdx     : end of data
# capture : done 49376 frames
```

`p 2` の録り終わりは**自動再生が次の曲へ送るのと同じ時刻**になる。どちらも
[§3.22](#322-曲の終わり方) の同じ状態を見ているため。演奏中に別の曲を `play` した
場合もそこで打ち切る（1 回のキャプチャは 1 曲ぶん）。曲を鳴らさないまま放置すれば
`p 0` を打つまで待ち続ける。

演奏の終端をループ回数で決めたいときは `vgm loop` / `mdx loop` と組み合わせる
（[§3.22](#322-曲の終わり方)）。ホスト側の `!capture-song`
（[docs/opm-writer.md](docs/opm-writer.md)）はこの手順をそのまま実行する。

`p 0` はドレインが終わってから `OK` を返す。待っている間も PCM の送出と USB の処理は
回り続けるので、DMA が止まることはない。2 秒で終わらなければ打ち切って
`ERR drain timeout` を返す（ホストが CDC #1 を読んでいない場合）。

**`p 0` は冪等。** 待機中に打っても `OK` を返す。DMA overrun で `ERROR` に落ちている
場合もここで待機へ戻せる（[§4.1](#41-取りこぼしたとき-dma-overrun)）。

エラーになる条件:

| 状況 | 応答 |
| --- | --- |
| キャプチャ中（`WAITING` を含む）に `p 1` / `p 2` | `ERR wrong state`（`# hint : already capturing; run p 0 first`） |
| `storage host` 中に `p 1` / `p 2` | `ERR wrong state`（`# hint : cannot capture PCM in storage host; run storage player first`。[§3.13](#313-storageストレージ)） |
| CDC #1 が開かれていない状態で `p 1` / `p 2` | `ERR not connected` |

いずれもコマンドの状態エラーであって、ファームウェアのエラー状態にはしない
（LED も変えない）。CDC #1 が開かれていないまま送信を始めるとリング 65.5ms 分で
必ず overrun するため、`p 1` / `p 2` の時点で弾いている。

キャプチャ中に CDC #1 が閉じられたら**即座に送信を停止**して待機へ戻る。これはエラーでは
ないので LED も変わらない。再接続しても自動では再開せず、次の `p 1` / `p 2` を待つ。
`p 2` の待機中（`WAITING`）に閉じられた場合も同じで、演奏を待たずに降りる。

### 3.11 `s`（統計）

`s` で実行時の統計を表示し、`s 0` でリセットする。`RING` と `USB_TX` の単位はバイト
（他の項目の単位は下の表のとおり）。**`s` のラベルだけは大文字**で、他のコマンドの
情報行は小文字にしてある。同じ値が両方に出ることがあるのはこのため
（`s` の `FLASH` と `storage status` の `# flash` は同じもの）。

```
# STATE   : CAPTURING
# CPU     : 40% (max 41%)   USB 8%
# RING    : 44/16384 bytes  MAX 568/16384  FREE 16340
# USB_TX  : 0/4096 bytes  MAX 1240/4096
# I2S     : depth 963/1024 frames  MIN 866  UNDERRUN 0
# OVERRUN : 0   E0 : 0   RXSTALL : 0
# RATE    : 62508 frames/s (expect 62500)
# QUIET   : 0 frames
# LOOP    : 468607 passes/s  MAX 1511 us  AUDIO GAP max 1561 us
# SVC     : pcm8 612/1991  cap 794/1991  i2s 781/1991  vgm 0/18430  mdx 110/18430  sto 0/998  worked/calls per s
# FRAMES  : 56263161
# FLASH   : WRITE 76   BLACKOUT max 42711 us
# VGM     : PLAYING AFTERBURNER.VGM
# VGM POS : 507150/2205000 samples  loop 7
# VGM LAG : reslip 0  gz reload 0
# MDX     : STOPPED
# MDX POS : 0 clocks  loop 0  ch 0
# MDX TICK: @t 200  14336 us  reslip 0
# MDX PCM : on  1/8 ch  mask 01  keyon 128  miss 0  reads 647  CLIP 2
# SEQ LAG : max 657 us
# PIOTEST : SKIP (disabled)
# IRQ     : H
OK
```

| 項目 | 内容 |
| --- | --- |
| `STATE` | キャプチャの状態。`IDLE` / `WAITING` / `CAPTURING` / `DRAINING` / `ERROR`（[§3.10](#310-ppcm-出力)） |
| `CPU` | 直近 1 秒のうち **サービス関数の中に居た時間**の割合と、リセット以降の最大値。`USB` は `tud_task()` の占有率で、毎周回走る固定費なので分けてある |
| `RING` | DMA リングの未処理量と high-water、空き |
| `USB_TX` | CDC #1 の送信バッファ滞留量と high-water。USB エンドポイントの状態ではなく、ファーム内の TX FIFO の滞留量 |
| `I2S` | I2S の DMA より先に書けているフレーム数と low-water、アンダーラン回数（[§5](#5-i2s-出力)）。ここだけは減る方向が危険なので最小値を残す |
| `OVERRUN` | DMA overrun の発生回数 |
| `E0` | YM3012 の禁止コード `E=0` を見た数。**PIO のビット位相が正しければ 0 のまま** |
| `RXSTALL` | PIO の RX FIFO があふれた回数。あふれると L/R の並びが崩れる |
| `RATE` | 直近 1 秒で数えた実測フレームレートと期待値 φM/64 |
| `QUIET` | DAC の出力が無音のまま続いているフレーム数。曲の終わりの余韻が消えたかを判定するのと同じ値で、鳴っていれば 0（[§3.22](#322-曲の終わり方)） |
| `LOOP` | 直近 1 秒のメインループ周回数。1 周あたりの固定費を見積もるのに使う。`MAX` は `service_all()` の呼び出し間隔の最大値、`AUDIO GAP max` は音声チェーンが実際に回った間隔の最大値で、**どちらもリアルタイムの余裕を直接表す**（[§3.11.1](#3111-サービスの呼び出し間隔)） |
| `SVC` | サービスごとの「実仕事をした回数 / 呼ばれた回数」（どちらも毎秒）。呼ばれた回数は[§3.11.1](#3111-サービスの呼び出し間隔)の間引き間隔で決まり、実仕事の側はその中で本当に処理があった回数。分子が分母に対して極端に小さければ、そのサービスは間引きを緩めても構わない |
| `FRAMES` | 取り込んだ総フレーム数 |
| `FLASH` | 内蔵フラッシュへ 4KiB ブロックを書き出した回数と、その間メインループが止まった最大時間（[§3.13](#313-storageストレージ)） |
| `VGM` | VGM の再生状態（`STOPPED` / `PLAYING` / `ERROR`）と再生中のファイル名。gzip 圧縮されたファイルなら末尾に `(gzip)` が付く（[§8.5](#85-vgzgzipの再生)） |
| `VGM POS` | 発行済みのサンプル位置 / ヘッダの総サンプル数と、ループした回数 |
| `VGM LAG` | VGM の時計を張り直した回数（[§3.14](#314-vgmvgm-再生)）。`gz reload` は `.vgz` のループで先頭から展開し直した回数で、**0 でなければループのたびに音が数百 ms 途切れている**（[§8.5](#85-vgzgzipの再生)） |
| `MDX` | MDX 再生の状態とファイル名（[§3.15](#315-mdxmdx-再生)）。`MDX_ENABLED=0` でビルドした場合は `DISABLED`（[§9.8](#98-無効化)） |
| `MDX POS` | 発行済みの clock 数、曲が何周したか、チャンネル数。`loop` は「まだ終わっていない全チャンネルがループ点に到達した」回数 |
| `MDX TICK` | 現在の Timer-B 値と 1 clock の長さ、時計を張り直した回数（[§9.2](#92-タイミング)） |
| `MDX PCM` | ADPCM ミキシングの有効・無効、発音中のチャンネル数とビットマスク、発音を開始した回数（`keyon`）と鳴らせなかった回数（`miss`）、PDX を読んだ回数、FM に足した結果あふれたサンプル数（[§9.7](#97-adpcm-pcm8-の再生)） |
| `SEQ LAG` | シーケンサが予定時刻から遅れた最大時間。**VGM と MDX で共用**（同時には再生できないので 1 個で足りる） |
| `PIOTEST` | 起動時の PIO ループバック自己診断の結果（[docs §4.6](docs/pico-opm-writer.md#46-起動時の自己診断)）。既定では `SKIP (disabled)`（[§5.4](#54-無効化)）。`i` と `t` では同じものが `# piotest` として出る |
| `IRQ` | OPM の /IRQ の現在のレベル |

`E0` / `RXSTALL` / `RATE` は、ロジックアナライザを繋がずにキャプチャ経路の健全性を
確かめるための指標。

**`CPU` は「残りどれだけ余裕があるか」の指標ではない。** メインループはやる事が無くても
回り続けるビジーループなので、1 周あたりの費用を削れば周回数 (`LOOP`) が増えて割合は
元に戻る。**この値だけを見て負荷が減った / 増えたと判断しないこと。** 実際に使っている
時間の絶対量を見たいときは `STATS_PROFILE=1` で焼く（[§3.11.1](#3111-サービスの呼び出し間隔)）。

**余裕の有無は `LOOP` の `MAX` / `AUDIO GAP max` と、`I2S` の `MIN` / `UNDERRUN`、
それに `RATE` で見ること。** 前者が原因（サービスが最大どれだけ空いたか）、
後者が結果（そのぶん先行量がどこまで削れたか）にあたる。`AUDIO GAP max` が
I2S の先行量 1024 フレーム（16.4ms）に近づいてきたら余裕が尽きかけている。

`s 0` は high-water / low-water とカウンタ（`LOOP MAX` / `AUDIO GAP max` / `OPMW` を
含む）に加えて、`MDX PCM` 行の `keyon` / `miss` / `reads` / `CLIP` も 0 に戻す（`keyon` / `miss` / `reads` は `mdx play` でも 0 に戻る）。

#### 3.11.1 サービスの呼び出し間隔

メインループは各サービスを**毎周回ではなく最短間隔を決めて**回す。DAC から届く
フレームは 62500/s なのに対しループは数十万周/秒あり、毎周回回すと 1 回あたり
1 フレーム未満のために毎回支度をすることになって、固定費だけで CPU の大半を使う。

| グループ | 間隔 | 決め手 |
| --- | --- | --- |
| `tud_task()` | 125µs | PCM 出力のパケットレート。64 バイト / 250KB/s = 256µs が下限で、その 2 倍 |
| 音声チェーン（リング取り込み / ADPCM / キャプチャ送出 / I2S 供給） | 500µs | 間隔を延ばすほど固定費は下がるが I2S の先行量の谷が深くなる。500µs で 31 フレーム |
| シーケンサ（VGM / MDX）と曲の終わり方・曲送り | 50µs | この間隔ぶんが `SEQ LAG` に乗る。曲の終わり方（[§3.22](#322-曲の終わり方)）と曲送り（autoplay）は同じ周回でシーケンサの後に、この順で見るので、更新された再生状態をそのまま読める |
| ストレージの書き出し | 1000µs | 判断はもともと 250ms のアイドル期限 |
| ボタンの取り込み（GP21 / GP22） | 1000µs | デバウンスの窓 10ms に対して 10 サンプル取れ、1 秒の長押し判定に対しては十分粗い（[§3.20](#320-ボタンgp21--gp22)） |

いずれもリング一周 65.5ms よりはるかに短いので、DMA の位置は見失わない。
値はソースの `AUDIO_SERVICE_INTERVAL_US` などの `#define` 1 箇所で決まる。

**ここに書いてあるのは「最短でもこの間隔を空ける」であって、上限ではない。**
コマンドの応答やシーケンサの予算ループで 1 周が伸びれば、実際の間隔はこれより
長くなる。**実際にどこまで空いたかは `s` の `LOOP MAX` と `AUDIO GAP max` に出る**
（実測で公称 500µs に対し 1.5ms 前後）。I2S の先行量 1024 フレーム = 16.4ms が
その上限なので、`AUDIO GAP max` がそこへ近づいてきたら間隔か処理量を見直すこと。

サービスごとの滞在時間を絶対量 (µs/s) で見たいときは、プロファイルを有効にして焼く。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 -DSTATS_PROFILE=1
```

`s` に `SVCTIME` と `OPMW` の 2 行が増える。どちらも区間ごとに時刻を 2 回読むぶん
本体より重くなるので、**常用はしない**。

```
# SVCTIME : pcm8 18302  cap 3042  i2s 23610  vgm 4886  mdx 34447  sto 169  us per s
# OPMW    : 727 writes/s  23407 us/s  avg 32.19 us  max 46 us
```

`OPMW` は OPM バスの 1 レジスタ書き込みの実費用。**`avg`（`us/s` ÷ `writes/s`）が
`opm_write()` 1 回の所要時間そのもの**なので、[opm.h](src/opm.h) のタイミング定数を
変えたときの前後比較はここで行う（[test/opm_busy/](test/opm_busy/README.md) の
ベンチマークと同じ量を実再生で測ったもの）。`max` は 1 回の最大値で、
USB の割り込みが挟まった回が出る。

### 3.12 `t`（自己テスト）

外部機器を使わずに実行できる自己テストをまとめて走らせる。

```
> t
# pcm     : PASS
# piotest : SKIP (disabled)
# mdx     : PASS (7 pitch, 5 tempo)
# adpcm   : PASS (19 cases)
OK
```

| 項目 | 内容 |
| --- | --- |
| `pcm` | PCM 変換の既知ベクタ検証。ゼロ / ±1 / 仮数境界 / 指数全域 / 禁止コード / 無効 3bit のマスク / 値域の両端。加えて全 `E`・全仮数でステップが `1 << (E-1)` になることを総当たりで確認する |
| `piotest` | 起動時に実施した PIO ループバック自己診断の結果（[docs §4.6](docs/pico-opm-writer.md#46-起動時の自己診断)）。**I2S が有効な既定構成では GP26-GP28 が競合するので実施せず `SKIP (disabled)` になる**（[§5.4](#54-無効化)） |
| `mdx` | MDX の音程 → KC/KF 変換と、Timer-B 値 → 1 clock の長さの既知ベクタ検証。KC の下位 4bit が 3/7/11/15 を飛ばす境界とオクターブ跨ぎを含む（[§9.4](#94-音程と音量の作り方)） |
| `adpcm` | MSM6258 の ADPCM デコーダの既知ベクタ検証。ステップ幅の表・符号ビット・12bit の飽和・段番号の上下端での頭打ちを含む。加えてレート比が全モードで整数であることと、音量 8 が原音（ゲイン 1.0）であることを確かめる（[§9.7](#97-adpcm-pcm8-の再生)） |

どれかが失敗したら `ERR self test failed` を返す。

機能をビルド時に落としてある場合、その項目は `SKIP (disabled)` になる（失敗ではない）。
`MDX_ENABLED=0` なら `mdx` と `adpcm` が、`PCM8_ENABLED=0` なら `adpcm` が
`SKIP (disabled)` になる（[§9.8](#98-無効化)）。

### 3.13 `storage`（ストレージ）

内蔵フラッシュ上の FAT ファイルシステムを、**Pico 側（FatFs）と PC 側（USB MSC）の
どちらが持つか**を排他で切り替える。詳しくは [§7](#7-ストレージ) を参照。

| コマンド | 説明 |
| --- | --- |
| `storage` / `storage status` | 現在の状態を表示する |
| `storage host` | フラッシュを PC へ渡す。PC にリムーバブルディスクとして現れる |
| `storage player` | フラッシュを Pico 側へ戻す。FatFs をマウントし直す |
| `storage format yes` | 領域を作り直す。既にファイルシステムがある場合は `storage format force yes` |
| `storage trace` | 直前に PC が投げた SCSI コマンドの記録を表示する（[§7.4](#74-マウントされないときの調べ方)） |

**`storage host` / `storage player` は冪等**で、既にそのモードなら何もせず `OK` を返す。
どちらも切り替え後のモードを 1 行出してから `OK` を返す。

**SW1 と SW2 の同時長押しでも `storage host` に入れる**（[§3.20](#320-ボタンgp21--gp22)）。
ボタン経由では自動再生・VGM・MDX を先に止めてから入るので、下記の拒否のうち残るのは
キャプチャ中だけになる。**HOST 中の LED は 200ms ON / 800ms OFF** になる
（[§3.9](#39-led)）ので、PC を見なくてもどちらのモードかが分かる。

```
> storage host
# storage : HOST
OK
```

`storage` / `storage status` の出力例:

```
# storage : PLAYER
# medium  : not present
# audio   : enabled
# region  : flash 0x040000 + 3836 KiB  (LBA 512 B x 7672)
# firmware: end 0x1001c654 (116308 B)  gap 142 KiB
# fs      : FAT12  cluster 4096 B  free 3808/3816 KiB
# label   : PICOOPM
# cache   : 8 lines  dirty 0
# flash   : WRITE 13   BLACKOUT max 39976 us
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

**`storage host` は、VGM 再生中・MDX 再生中・PCM キャプチャ中だと `ERR wrong state` で
拒否する。** どれで落ちたかは直前の `# hint` 行に出る。先に `vgm stop` / `mdx stop` /
`p 0` を実行すること（[§3.19](#319-状態による拒否の一覧)）。

```
> storage host
# hint    : cannot switch while VGM is playing; run vgm stop first
ERR wrong state
```

`HOST` 中は次のものが使えない。フラッシュの消去でメインループが数十 ms 止まり、
キャプチャの DMA リング（65.5ms 分）と I2S の先行量（16.4ms 分）を守れないため。

* `p 1`（PCM キャプチャ）
* I2S 出力（無音になる。BCK / LRCK は止めないので DAC はポップしない）
* `vgm list` / `vgm play` / `mdx list` / `mdx play`（ファイルシステムがアンマウントされている）
* `storage format`

`PLAYER` へ戻した時点で音声経路を復帰させ、キャプチャと I2S のリング位置を張り直す。

### 3.14 `vgm`（VGM 再生）

`/VGM/` に置いた VGM ファイルを再生する。演奏対象は **YM2151 の部分だけ**で、
他の音源のコマンドは読み飛ばす。gzip 圧縮された `.vgz` も一時ファイルを作らずに
そのまま再生できる（[§8.5](#85-vgzgzipの再生)）。詳しくは [§8](#8-vgm-再生) を参照。

| コマンド | 説明 |
| --- | --- |
| `vgm` / `vgm status` | 再生状態を表示する |
| `vgm list` | `/VGM/` **以下**の `.vgm` と `.vgz` を並べる。サブフォルダも辿る |
| `vgm play <path>` | `/VGM/<path>` を再生する。`/VGM/` は付けない。`<path>` は `/VGM/` からの相対パス |
| `vgm stop` | 再生を止めて全チャンネルをキーオフする。RR を 15 にしてから落とすので速やかに消える。**冪等**（停止中でも `OK`。曲が自然に終わったあとの余韻もこれで消せる） |
| `vgm loop` / `vgm loop <n>` | 何周したらフェードアウトして終わるかの表示 / 設定。10 進、`0`-`99`。**`0` は無限**（既定）で、ループを持つ曲は止まらない |
| `vgm fade` / `vgm fade <ms>` | フェードアウトの長さの表示 / 設定。10 進、`0`-`60000`（既定 `2000`）。`0` はフェードせず即停止 |

```
> vgm
# vgm     : PLAYING AFTERBURNER.VGM
# pos     : 507150/2205000 samples  loop 7
# lag     : reslip 0  gz reload 0
# end     : loop endless  fade 2000 ms
# song    : PLAYING
OK
```

`.vgz` を再生中なら 1 行目の末尾に `(gzip)` が付く。`# pos` / `# lag` と同じ内容は
`s` の `VGM` / `VGM POS` / `VGM LAG` の 3 行にもある（[§3.11](#311-s統計)）。
`# end` は上の `vgm loop` / `vgm fade` の現在値、`# song` は曲の終わり方の状態で、
どちらも [§3.22](#322-曲の終わり方) を参照。

```
> vgm loop 2
# end     : loop 2  fade 2000 ms
OK
```

```
> vgm list
# file    :   1234567 AFTERBURNER.VGM
# file    :    234567 OUTRUN.VGZ
# file    :    345678 KONAMI/GRADIUS.VGM
# file    :    456789 KONAMI/OLD/TWINBEE.VGM
# file    :    567890 SEGA/OUTRUN.VGM
# files   : 5
OK

> vgm play KONAMI/GRADIUS.VGM
# vgm     : version 1.51  samples 2205000  loop yes
# clock   : file 3579545 Hz / phiM 4000000 Hz (pitch goes up)
OK
```

サイズを先に置き、ファイル名を必ず最後の欄にしてある（名前に空白を含みうるため）。
`.vgz` のサイズは圧縮された状態のバイト数。

#### サブフォルダの扱い

`vgm list` / `mdx list` は `/VGM/` `/MDX/` の**下の階層も辿る**。出す名前はルートからの
相対パスで、区切りは `/`。並びは**深さ優先**で、あるフォルダのファイルを名前順（大小無視）に
出し切ってから、サブフォルダを名前順に 1 つずつ降りる。上の例なら `KONAMI/GRADIUS.VGM` が
`KONAMI/OLD/TWINBEE.VGM` より先に来る。

`vgm play` / `mdx play` にも同じ相対パスをそのまま渡せる。上限は 2 つ:

| 制限 | 値 |
| --- | --- |
| 相対パスの長さ | 127 文字 |
| 階層の深さ | 8 段（`/VGM` 自身を 1 段目と数えるので、その下は 7 段まで） |

超えたものは一覧にもプレイリストにも出さず、`# files` 行の前に警告を出す。

```
# warn    : skipped 3 path(s) longer than 127 chars
# warn    : skipped 1 directory(s) deeper than 8 levels
```

`.` で始まるフォルダ（macOS が作る `.Spotlight-V100` など）と、隠し属性・システム属性の
付いたフォルダには潜らない。

`vgm list` は 256 件で打ち切る。打ち切ったときは `# files` 行の前に警告が出る
（`mdx list` も同じ上限）。

```
# warn    : truncated at 256 entries
# files   : 256
```

デュアルチップの VGM は 2 個目のチップを無視するので、`vgm play` が警告を出す
（[§8.1](#81-対応範囲)）。

```
# warn    : dual chip file; the second chip (0xA4) is ignored
```

`.vgz` を再生すると `# vgm` 行の末尾に `gzip` が付く。

```
> vgm play OUTRUN.VGZ
# vgm     : version 1.51  samples 1852000  loop yes  gzip
OK
```

**再生中は `w` / `reset` / `c` と `storage host` を `ERR wrong state` で拒否する。**
VGM とユーザーのレジスタ書き込みが混ざると何が鳴っているのか分からなくなるため。
`p 1` / `p 0` / `s` / `i` / `t` / `d` は再生中も使える（VGM を鳴らしながら
CDC #1 へ録音できる）。全体は [§3.19](#319-状態による拒否の一覧)。

**`vgm play` / `mdx play` は再生中でも受け付ける。** 走っている方を止めてから
新しいファイルを開くので、曲を変えるのは 1 コマンドで済む。止めた曲は 1 行で報告する。

```
> vgm play OUTRUN.VGM
# vgm     : stopped AFTERBURNER.VGM
# vgm     : version 1.51  samples 1852000  loop yes
OK
```

**新しいファイルを開けなかったときは前の曲へは戻らず、停止状態で終わる。**
`stopped` の行が出るので、応答だけで前の曲が止まったことが分かる。

```
> vgm play NOSUCH.VGM
# vgm     : stopped OUTRUN.VGM
ERR not found
```

ファイル名の書式が不正なとき（`ERR bad argument`）だけは、ファイルを開く前に弾くので
前の曲がそのまま鳴り続ける。

ループを持たないファイルはデータの終端で自動的に止まる。全チャンネルをキーオフして
ファイルを閉じ、状態は `STOPPED` に戻る。再生を始めたあとにファイルの中身が壊れて
いると分かった場合は、`OK` を返したあとなので非同期通知になり、状態は `ERROR` になる。
どちらも [§3.18](#318-非同期通知) を参照。

### 3.15 `mdx`（MDX 再生）

`/MDX/` に置いた MDX ファイル（X68000 の MXDRV 用のバイナリ MML）を再生する。
**FM 8ch と ADPCM 8ch** を鳴らす。ADPCM は本機にハードウェアが無いのでソフトウェアで
デコードし、FM の出力に足して I2S と PCM キャプチャへ流す。
詳しくは [§9](#9-mdx-再生) を参照。

| コマンド | 説明 |
| --- | --- |
| `mdx` / `mdx status` | 再生状態を表示する |
| `mdx list` | `/MDX/` **以下**の `.mdx` を並べる。サブフォルダも辿る。256 件で打ち切る |
| `mdx play <path>` | `/MDX/<path>` を再生する。`/MDX/` は付けない。`<path>` は `/MDX/` からの相対パス |
| `mdx stop` | 再生を止めて全チャンネルをキーオフする。RR を 15 にしてから落とすので速やかに消える。**冪等**（停止中でも `OK`。曲が自然に終わったあとの余韻もこれで消せる） |
| `mdx loop` / `mdx loop <n>` | 何周したらフェードアウトして終わるかの表示 / 設定。10 進、`0`-`99`。**`0` は無限**（既定） |
| `mdx fade` / `mdx fade <ms>` | フェードアウトの長さの表示 / 設定。10 進、`0`-`60000`（既定 `2000`）。`0` はフェードせず即停止 |
| `mdx pcm` | ADPCM ミキシングの状態を表示する |
| `mdx pcm on` / `mdx pcm off` | ADPCM を足す / 足さない（FM だけの音と聴き比べる用） |

```
> mdx
# mdx     : PLAYING GRADIUS.MDX
# title   : グラディウス / KONAMI
# pos     : 12345 clocks  loop 0  ch 9
# tick    : @t 200  14336 us  reslip 0
# end     : loop endless  fade 2000 ms
# song    : PLAYING
OK
```

同じ内容は `s` の `MDX` / `MDX POS` / `MDX TICK` の 3 行にもある
（[§3.11](#311-s統計)）。ADPCM の詳細は `mdx pcm` の方に出る。

```
> mdx list
# file    :      8192 GRADIUS.MDX
# file    :     12345 XEVIOUS.MDX
# file    :      6543 ZOOM/OVERTAKE.MDX
# files   : 3
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
# pdxpath : /MDX/THEXDER.PDX
OK
```

`# pdx` は MDX のヘッダが要求している名前、`# pdxpath` は実際に開いたファイル。
探す順と置き方は [§9.7](#97-adpcm-pcm8-の再生)。

PDX が見つからないときはエラーにはならず、FM パートだけがそのまま鳴る。

```
# pdx     : THEXDER
# hint    : cannot open /MDX/THEXDER.PDX (not found); the ADPCM part will not sound
```

`mdx pcm` で ADPCM の状態が見える。

```
> mdx pcm
# adpcm   : on
# pdxpath : /MDX/THEXDER.PDX
# active  : 2 ch  mask 03  pan L+R
# keyon   : 128   miss 0
# reads   : 647   clip 0
OK
```

`keyon` は ADPCM の発音を開始した回数、`miss` は鳴らそうとして波形が見つからなかった
（または音量 0 になった）回数。**ADPCM が終盤にしか出てこない曲もある**ので、
「聞こえない」だけでは曲の側の話かファームの側の話か分からない。この 2 つを見れば
聴かずに切り分けられる。カウンタは `mdx play` と `s 0` で 0 に戻り、曲が終わったあとも残る。

`mdx pcm off` は**電源を切るまで残る**。off のまま PDX を要求する曲を再生すると、
`mdx play` が理由を出す。

```
> mdx play THEXDER.MDX
...
# pdxpath : /MDX/THEXDER.PDX
# hint    : ADPCM mixing is off; run mdx pcm on to restore it
```

**再生中は `w` / `reset` / `c` と `storage host` を `ERR wrong state` で拒否する。**
`p 1` / `p 0` / `s` / `i` / `t` / `d` は再生中も使える。
全体は [§3.19](#319-状態による拒否の一覧)。

**`mdx play` は再生中でも受け付ける**（[§3.14](#314-vgmvgm-再生) と同じ）。VGM と MDX を
同時に鳴らすことはできないが、どちらが鳴っていても `play` が止めてから始めるので、
`mdx play` ↔ `vgm play` の行き来も 1 コマンドでできる。

`.mdx` として読めない中身は `ERR bad file`。64KiB を超えるファイルも受け付けない
（`# hint : MDX must be 65536 bytes or less`）。再生を始めたあとに壊れていると
分かった場合は `OK` を返したあとなので非同期通知になる。このとき全チャンネルを
キーオフして PDX も閉じ、状態は `ERROR` になる。

全チャンネルが演奏終了（`0xF1 0x00`）に達すると自動で止まる。ループを持つ曲は
止まらないので `mdx stop` で止める。どの止まり方も 1 行の非同期通知が出る
（[§3.18](#318-非同期通知)）。

### 3.16 `clock`（クロック切り替え）

φM を動作させたまま切り替える。プリセットは [§2](#2-クロック設定-φm) の 2 つ。

| 書式 | 動作 |
| --- | --- |
| `clock` / `clock status` | 現在の φM / sys_clk / プリセット / I2S のレートを表示する |
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

既に同じプリセットのときは何もせずに応答だけを返す（冪等）。プリセット名以外を
渡すと `ERR unknown command`（`clock 5` / `clock 3.6` など）。

**PCM を流している間（`CAPTURING` / `DRAINING`）は `ERR wrong state` で拒否する。**
サンプリングレートがストリームの途中で変わると、ホスト側で出来上がる WAV の時間軸が
黙って狂うため。これは `vgm play` の自動追従にも同じように効き、キャプチャ中に
別クロックの VGM を再生しようとすると再生自体が `ERR wrong state` になる
（同じクロックなら通る）。このとき前の曲は止まったままになる（[§3.14](#314-vgmvgm-再生)）。

```
> clock 3.58
# hint    : cannot switch phiM while capturing PCM; run p 0 first
ERR wrong state
```

**`p 2` の待機中（`WAITING`）は通す。** まだ 1 フレームも流していないので、途中で
レートが変わるストリームがそもそも存在しない。通さないと、ファイル側のクロックを
要求する VGM を `p 2` で録ろうとした時点で `vgm play` が必ず弾かれる
（[docs/opm-record.md](docs/opm-record.md) が使うのはこの経路）。

VGM 再生中は拒否しない。レジスタを叩くわけではなく、変わるのは音程と包絡線の速さだけで、
テンポは 44100Hz の絶対サンプル数で刻んでいるので狂わない。

### 3.17 `autoplay`（自動再生）

`/VGM/` と `/MDX/` のファイルを 1 本のプレイリストに束ねて順に鳴らす。曲が終わったこと・
指定した周回数に達したことをファームウェア側で見張るので、ホストは `autoplay start` を
1 回打つだけでよい。

曲の終わり方そのものは [§3.22](#322-曲の終わり方) の状態機械が持っていて、こちらは
その `IDLE` を待って次の曲へ送るだけ。`autoplay loop` / `autoplay fade` は
自動再生中だけ `vgm loop` / `mdx loop` の代わりに使われる（既定値が違うので
2 系統に分けてある。手動再生は `0` = 無限、自動再生は `2`）。

| コマンド | 説明 |
| --- | --- |
| `autoplay` / `autoplay status` | 状態を表示する |
| `autoplay list` | プレイリストを鳴らす順に並べる。現在の曲に `*` が付く |
| `autoplay start` | プレイリストを作り直して 1 曲目を鳴らす（開始時は間隔を空けない） |
| `autoplay stop` | 自動再生を止める。鳴っている曲も止まる。**冪等**（停止中でも `OK`） |
| `autoplay next` / `autoplay prev` | 次 / 前の曲へ**即座に**移る。フェードも間隔も挟まない |
| `autoplay mode list` / `autoplay mode random` | 曲順。既定は `list` |
| `autoplay loop <n>` | フェードを始めるまでの周回数。`0`-`99`、既定 `2`。`0` は無限 |
| `autoplay fade <ms>` | フェードアウトの長さ。`0`-`60000`、既定 `2000`。`0` はフェードせず即停止 |
| `autoplay gap <ms>` | 曲間の無音。`0`-`60000`、既定 `2000` |
| `autoplay source vgm` / `mdx` / `both` | 対象ディレクトリ。既定は `both` |

```
> autoplay
# autoplay: PLAYING  mode list  source both
# song    : PLAYING
# track   : 3/105  mdx BOS01.MDX
# timing  : loop 2  fade 2000 ms  gap 2000 ms
OK
```

`# song` は鳴っている曲の終わり方の状態で、`vgm status` / `mdx status` に出るものと
同じ（[§3.22](#322-曲の終わり方)）。`# autoplay` が `PLAYING` のままでも、
`# song` は `FADING` や `RINGOUT` を通って `IDLE` になり、そこで次の曲へ送られる。

```
> autoplay list
# entry   : *   1 vgm DEMO.VGM
# entry   :     2 vgm LOOPTEST.VGM
# entry   :     3 vgm KONAMI/GRADIUS.VGM
# entry   :     4 mdx BOS01.MDX
...
# entries : 105
OK
```

#### 曲順

`list` は `/VGM/` 以下の全曲に続けて `/MDX/` 以下の全曲を並べる。それぞれの中は
`vgm list` / `mdx list` と同じ**深さ優先**（[§3.14](#314-vgmvgm-再生)）で、曲名はルートからの
相対パス。`vgm list` と `mdx list` の出力をそのまま繋いだ順になり、`autoplay list` の
並びと一致する。

`random` は `autoplay start` のたびにシャッフルし、最後まで行くと並べ直す。並べ直した
直後に同じ曲が 2 回続かないようにしてある。

プレイリストは `autoplay start` のときだけ作る。`autoplay source` や `autoplay mode` を
変えたあと、あるいは PC からファイルを足したあとは、`autoplay start` を打ち直す。
上限は 512 件で、相対パスの合計が 24KiB を超えるか 512 件に達すると
`# warn    : truncated at <件数> entries` を出して打ち切る。パスが 127 文字を超える曲と
8 段より深いフォルダは `vgm list` / `mdx list` と同様にプレイリストにも入らず、
`autoplay start` が同じ `# warn` を出す（[§3.14](#314-vgmvgm-再生)）。

#### 次の曲へ移る条件

**`# song` が `IDLE` になったら次へ送る**（[§3.22](#322-曲の終わり方)）。そこへ至る道は 2 つ。

- **曲が終わった。** ループを持たない VGM の終端、MDX の全チャンネルの演奏終了、
  どちらも再生系が自分で止まるのをそのまま使う。再生中に壊れていると分かって
  `ERROR` に落ちた場合も次へ送る。
- **`autoplay loop <n>` の周回数に達した。** ループカウンタが `n` に達した時点
  （`n` 周を弾き終えて `n+1` 周目に入った瞬間）からフェードアウトを始め、
  `autoplay fade <ms>` の時間をかけて落としてから止める。`autoplay loop 0` に
  すると周回では止めず、曲の終端でだけ次へ進む。

どちらの場合も、**曲の余韻（`RINGOUT`）が消えるのを待ってから** `autoplay gap <ms>` の
無音を数え始める。曲間の無音は「余韻 → 間隔」の順で、次の曲の頭に前の曲の尾が被らない。

```
# mdx     : end of data
# mdx     : BOS02.MDX
...
# song    : fade out (loop 2)
# song    : end of fadeout
# vgm     : version 1.51  samples 2205000  loop yes
...
```

#### フェードアウトの効く範囲

フェードは **YM3012 から取り込んだ PCM に掛けるデジタルゲイン**で作る。ADPCM を混ぜた
あとの 1 箇所に掛かるので、**I2S 出力（[§5](#5-i2s-出力)）と PCM キャプチャ
（[§4](#4-pcm-出力)）の両方に等しく効く**。

**基板上の YM3012 のアナログ出力には効かない。** Pico は OPM から DAC へのシリアル線を
傍受しているだけで、その経路には入っていない（[§1.1](#11-接続表)）。アナログ側で音が
止まるのは、フェードが終わったあとのキーオフの時点になる。

ゲインを 1.0 へ戻すのはキーオフの後。ただし**キーオフしてもチップはすぐには黙らない**
（全スロットの RR を 15 にしても、リリースが消えるまで最悪 5.5ms かかる）ので、
16ms の猶予を置いてから 4ms かけて戻す。そのぶん曲間の無音が伸びるだけで、次の曲は
`autoplay gap 0` や `autoplay next` でも頭を削られずに始まる。

#### 他のコマンドとの関係

**手で `vgm play` / `mdx play` / `vgm stop` / `mdx stop` を打つと自動再生は降りる。**
選んだ曲だけが鳴り、プレイリストの続きへは進まない。

```
> vgm play TESTTONE.VGM
# autoplay: stopped
# vgm     : version 1.51  samples 66150  loop no
OK
```

**曲を開けなかったときは 1 行出して次の曲へ送る。** プレイリストを一巡しても 1 曲も
鳴らせなければ自動再生ごと止める。キャプチャ中（`p 1`）に φM の違う曲へ進もうとすると
`clockmode` が拒否するので、この経路は普通に通る（[§3.16](#316-clockクロック切り替え)）。

```
# hint    : cannot switch phiM while capturing PCM; run p 0 first
# autoplay: skip BOS01.MDX (wrong state)
...
# autoplay: stopped (no playable file)
ERR wrong state
```

**`storage host` は自動再生中なら拒否する。** 曲間（`GAP`）は VGM も MDX も鳴っていないが、
通したところで次の曲で必ず失敗するので、`autoplay stop` を先に打たせる
（[§3.19](#319-状態による拒否の一覧)）。

設定（`mode` / `source` / `loop` / `fade` / `gap`）は `autoplay stop` では消えず、
電源を入れ直すと既定値に戻る。

#### ボタンからの操作

基板上の SW1 / SW2 で、PC を繋がずに開始・曲順の切り替え・曲送りができる。
どの押し方が何に相当するかは [§3.20](#320-ボタンgp21--gp22) の表にまとめてある。

**長押しは `autoplay start` なので、プレイリストを作り直して 1 曲目から始まる。**
`list` で再生中に SW1 を長押しすると先頭へ戻る。曲順を変えずに送りたいだけなら短押しを使う。

#### 無効化

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 -DAUTOPLAY_ENABLED=0
```

プレイリスト（約 26KiB）と状態機械がリンクされず、`autoplay` は `ERR unsupported` を
返すようになる。

```
> autoplay start
# hint    : autoplay is disabled at build time (AUTOPLAY_ENABLED=0)
ERR unsupported
```

### 3.18 非同期通知

コマンドの応答とは無関係に、ファームウェア側の都合で出る 1 行の通知。**すべて `#` で
始まる情報行**なので、「1 コマンド 1 応答」（[§3.3](#33-応答)）は崩れない。ホスト側は
情報行として読み飛ばすか、必要なら内容を見ればよい。

| 通知 | 意味 |
| --- | --- |
| `# vgm     : end of data` | ループを持たない VGM がデータの終端に達して止まった（[§3.14](#314-vgmvgm-再生)） |
| `# mdx     : end of data` | MDX の全チャンネルが演奏終了（`0xF1 0x00`）に達して止まった（[§3.15](#315-mdxmdx-再生)） |
| `# mdx     : end of fadeout` | MDX が MML のフェードアウト（`0xE7 0x01`）を完了して止まった |
| `# song    : fade out (loop <n>)` | ループ回数が上限に達したのでフェードアウトを始めた（[§3.22](#322-曲の終わり方)） |
| `# song    : end of fadeout` | 上のフェードアウトが終わって演奏を止めた |
| `# warn    : ringout cut at <ms> ms` | 曲の終わりの余韻が上限まで消えなかったので強制的に消音した（[§3.22](#322-曲の終わり方)） |
| `# warn    : ringout gave up at <ms> ms` | 強制消音しても無音にならなかったので余韻を待つのをやめた（[§3.22](#322-曲の終わり方)） |
| `# autoplay: skip <名前> (<理由>)` | 自動再生がその曲を開けなかったので次へ送った |
| `# autoplay: stopped` | 自動再生が降りた。手動の `vgm play` / `mdx play` / `*_stop` でも出る |
| `# autoplay: stopped (no playable file)` | プレイリストを一巡して 1 曲も鳴らせなかった |
| `# storage : ejected by host` | PC がリムーバブルディスクを取り出した。所有権が Pico 側へ戻る（[§7.2](#72-pc-から曲データをコピーする)） |
| `# button  : <ボタン> <長さ>: <実行した内容>` | ボタンの操作を受け付けた（[§3.20](#320-ボタンgp21--gp22)） |
| `# button  : <操作> failed (<理由>)` | ボタンの操作が状態の制約で通らなかった。直前に各モジュールの `# hint` が出る |
| `# capture : done <n> frames` | `p 2` のキャプチャが曲の終わりで自分から止まった（[§3.10](#310-ppcm-出力)） |
| `# capture : abort (<理由>)` | `p 2` のキャプチャが曲の終わりを待たずに打ち切られた。`done` は出ない（[§4.1](#41-取りこぼしたとき-dma-overrun)） |
| `# ERR dma overrun` | キャプチャのリングがあふれて送信を止めた。状態は `ERROR`（[§4.1](#41-取りこぼしたとき-dma-overrun)） |
| `# ERR vgm bad file (<理由> at 0x<位置>)` | 再生中の VGM が壊れていた。状態は `ERROR`（[§3.14](#314-vgmvgm-再生)） |
| `# ERR mdx bad file (<理由> at 0x<位置>)` | 再生中の MDX が壊れていた。状態は `ERROR`（[§3.15](#315-mdxmdx-再生)） |
| `# ERR storage io error` | フラッシュへの書き出しに失敗した（[§7.3](#73-書き込みの仕組みと制約)） |
| `# ERR storage region overlaps firmware (...)` | 起動時。FatFs 領域がファームウェアと重なっているのでマウントも書き込みもしない（[§7.1](#71-領域の変え方)） |

```
# ERR vgm bad file (opcode 0x2f at 0x0001a34c)
# ERR mdx bad file (truncated at 0x00000c34)
# button  : SW1 short: next track
# button  : SW1+SW2 long: storage host
# button  : storage host failed (wrong state)
```

**`# ERR ...` は裸の `ERR ...`（コマンドの応答）とは別物。** 頭の `# ` の有無で
機械的に区別できる。`ERROR` に落ちた状態は `s`（[§3.11](#311-s統計)）や
`vgm` / `mdx` / `p` の状態表示からも読める。

### 3.19 状態による拒否の一覧

`ERR wrong state` になる組み合わせ。**拒否されたときは直前に必ず `# hint` 行が出る**ので、
どの原因で落ちたかは応答だけで分かる。原因は下の 4 列に**自動再生中**を加えた 5 つで、
5 つ目が効くのは `storage host` だけ（下記）。

| コマンド | VGM 再生中 | MDX 再生中 | キャプチャ中 (`p 1` / `p 2`) | `storage host` 中 |
| --- | --- | --- | --- | --- |
| `w` / `reset` / `c` | 拒否 | 拒否 | 可 | 可 |
| `r 0` / `r 1` | 可 | 可 | 可 | 可 |
| `d` / `i` / `t` / `s` / `h` | 可 | 可 | 可 | 可 |
| `p 1` / `p 2` | 可 | 可 | 拒否 | 拒否 |
| `p 0` | 可 | 可 | 可 | 可 |
| `clock 4` / `clock 3.58` | 可 | 可 | **拒否**（`p 2` の待機中は可） | 可 |
| `clock auto` / `clock fixed` | 可 | 可 | 可 | 可 |
| `storage host` | 拒否 | 拒否 | 拒否 | 可（冪等） |
| `storage player` | 可 | 可 | 可 | 可 |
| `storage format` | 可 | 可 | 可 | 拒否 |
| `vgm list` / `mdx list` | 可 | 可 | 可 | 拒否 |
| `vgm play` / `mdx play` | 可（止めて始める） | 可（止めて始める） | 同じ φM なら可 | 拒否 |
| `vgm stop` / `mdx stop` | 可 | 可 | 可 | 可 |
| `autoplay start` | 可（止めて始める） | 可（止めて始める） | 同じ φM なら可 | 拒否 |
| `autoplay next` / `autoplay prev` | 可 | 可 | 同じ φM なら可 | 拒否 |
| `autoplay stop` | 可 | 可 | 可 | 可 |
| `autoplay list` と設定（`mode` / `loop` / `fade` / `gap` / `source`） | 可 | 可 | 可 | 可 |
| 状態表示（`p` / `clock` / `storage` / `vgm` / `mdx` / `mdx pcm` / `autoplay`） | 可 | 可 | 可 | 可 |

読み方の要点:

- **`w` / `reset` / `c` を再生中に拒否する**のは、シーケンサとユーザーの書き込みが混ざると
  何が鳴っているのか分からなくなるため（[§3.14](#314-vgmvgm-再生)）。**読み出しの `r` は
  拒否しない**（レジスタを変えないので、再生中のステータスをそのまま観測できる。
  [§3.21](#321-r読み出し)）。
- **`storage host` を 3 つの状態すべてで拒否する**のは、フラッシュの消去でメインループが
  数十 ms 止まり、キャプチャの DMA リング（65.5ms 分）と I2S の先行量（16.4ms 分）を
  守れないため（[§3.13](#313-storageストレージ)）。
- **キャプチャ中の `clock` 切り替えを拒否する**のは、サンプリングレートがストリームの
  途中で変わるとホスト側で出来上がる WAV の時間軸が黙って狂うため
  （[§3.16](#316-clockクロック切り替え)）。`vgm play` / `mdx play` の自動追従にも
  同じように効くので、**キャプチャ中に別クロックの曲を再生しようとすると再生自体が
  拒否される**（同じクロックなら通る）。**このとき前の曲は既に止まっている。**
- **`p 2` の待機中（`WAITING`）だけは φM の列から外れる。** まだ 1 フレームも
  流していないので `clock 4` / `clock 3.58` も、別クロックの `vgm play` / `mdx play` /
  `autoplay` も通る（[§3.10](#310-ppcm-出力)）。それ以外の列 — `p 1` / `p 2` の拒否も
  `storage host` の拒否も — は待機中かどうかに関わらず表のとおり。
- **`storage host` だけは 5 つ目の原因「自動再生中」も見る。** 曲間（`GAP`）は VGM も
  MDX も鳴っていないので表の 2 列では素通りしてしまうが、通しても次の曲で必ず失敗する
  （[§3.17](#317-autoplay自動再生)）。他のコマンドは自動再生そのものを見ておらず、
  曲が鳴っていれば VGM / MDX の列で、鳴っていなければ通る。
- **`vgm play` / `mdx play` は再生中でも拒否しない。** 走っている方を止めてから
  新しいファイルを開く。開いたあとで失敗したときは前の曲へは戻らず、停止状態で
  `ERR` を返す（[§3.14](#314-vgmvgm-再生)）。
- **`stop` 系と `storage` のモード切り替えは拒否されない。** いずれも冪等で、
  既にその状態でも `OK` を返す（[§3.4](#34-コマンド一覧)）。
- **`autoplay list` と設定系はどの状態でも通る。** どちらも RAM のプレイリストと
  設定値を触るだけで、ファイルシステムを読まないため。**プレイリストの作り直し
  （`autoplay start`）だけはファイルシステムが要る**ので `storage host` 中は拒否される。
- **ボタン経由でも同じ表が効く。** ただしボタンの `storage host` は自動再生・VGM・MDX を
  先に止めてから入るので、**残る拒否要因はキャプチャ中だけ**になる
  （[§3.20](#320-ボタンgp21--gp22)）。キャプチャを止めないのは、`p 1` 中は必ずホストが
  CDC を握っていて `p 0` を打てるため。

ビルド時に落とした機能は `ERR wrong state` ではなく `ERR unsupported` を返す
（[§9.8](#98-無効化)）。

### 3.20 ボタン（GP21 / GP22）

基板上の 3 個のタクトスイッチで、**PC を繋がずに**自動再生とストレージを操作する。

| ボタン | 接続先 | 役割 |
| --- | --- | --- |
| SW1 | GP21 | 自動再生の開始（`list`）と次の曲 |
| SW2 | GP22 | 自動再生の開始（`random`）と前の曲 |
| SW3 | RUN | ハードウェアリセット。**ファームウェアからは見えない** |

SW1 / SW2 は GP21 / GP22 と GND の間に入れるだけでよい（[§1.3](#13-配線上の注意)）。
RP2350 の内部プルアップを張って負論理で読み、チャタリングは時間で除去する。

#### 起動時のモード選択

**ボタンを押しながら起動すると、そのモードで動き始める。** 押しながら USB に電源を
入れるか、SW1 / SW2 を押しながら SW3 でリセットする。

| 押されているボタン | 動作 | LED |
| --- | --- | --- |
| SW1 のみ | `autoplay mode list` + `autoplay start` 相当 | 1 回点滅の繰り返し |
| SW2 のみ | `autoplay mode random` + `autoplay start` 相当 | 2 回点滅の繰り返し |
| SW1 + SW2 | `storage host` 相当 | 3 回点滅の繰り返し |
| なし | 通常起動（従来どおり） | 常時点灯 |

**モードは起動した瞬間の押下で決まり、実行は両方を離してから**行う。離すまでは LED が
上記のパターンで点滅し続けるので、意図したモードになっているかを確かめてから離せる
（違っていたら SW3 で起動し直す）。離したあとに押し足しても、決まったモードは変わらない。

起動時の出力は**ホストが CDC を開く前なので捨てられる**。結果は `i` の `# button` 行に
残るので、後から USB を挿しても分かる（[§3.6](#36-i情報表示の出力例)）。

ファイルシステムが無い基板で SW1 / SW2 起動しても、自動でフォーマットはしない
（`# button : long 1000 ms boot autoplay list (no filesystem)` が残るだけ）。
**SW1+SW2 の `storage host` は未フォーマットでも通る**ので、新品の基板を PC 側で
フォーマットする用途に使える。

#### 動作中の操作

**長押しのしきい値は 1 秒。** 押し続けてしきい値に達した瞬間に成立し、LED が
100ms OFF → 500ms ON → 100ms OFF の合図を出す。**離すのを待たない**ので、
効いたかどうかが押している最中に分かる。合図が出たあとは離すまで何も起きない。
しきい値未満で離した場合は短押しになり、離した瞬間に効く。

| 操作 | 動作 |
| --- | --- |
| SW1 長押し | `autoplay mode list` + `autoplay start` 相当 |
| SW2 長押し | `autoplay mode random` + `autoplay start` 相当 |
| SW1 + SW2 長押し | `storage host` 相当 |
| SW1 短押し | `autoplay next` 相当。**自動再生が止まっていれば SW1 長押しと同じ** |
| SW2 短押し | `autoplay prev` 相当。**自動再生が止まっていれば SW2 長押しと同じ** |
| SW1 + SW2 短押し | **何もしない**（1 行出すだけ） |
| SW3 | 押し下げ時間に関わらずハードウェアリセット |

**長押しでモードを切り替えるときは、元のモードを破棄する。** `storage host` へ入る前に
自動再生と VGM / MDX の再生を止め、自動再生を始める前に `storage player` へ戻す。

**SW1 + SW2 の短押しに動作を割り当てていない**のは、`storage host` が破壊的だから。
曲送りを速く連打したときに誤ってファイルシステムを PC へ渡してしまわないよう、
`storage host` は必ず 1 秒の長押しを要求する。

**`storage host` 中は短押しが効かない。** 抜けられるのは長押しか SW3 のリセットだけ
（[§7.2](#72-pc-から曲データをコピーする)）。

```
# button  : SW1 short: next track
# button  : SW2 long: autoplay random
# button  : SW1+SW2 long: storage host
# button  : SW1+SW2 short: ignored (hold 1 s for storage host)
# button  : SW1 short: ignored (storage is handed to the PC)
```

状態の制約で通らなかったときは、各モジュールの `# hint` に続けて 1 行出す。
拒否の条件はコマンドで打った場合と同じ（[§3.19](#319-状態による拒否の一覧)）。

```
# hint    : cannot switch while capturing PCM; run p 0 first
# button  : storage host failed (wrong state)
```

#### 同時押しの判定

**両方を押すタイミングがずれてもよい。** 押している間に押されたボタンをすべて覚え、
**2 個目が入った時点から 1 秒**を数え直す。SW1 を押してから SW2 を足すまでに
0.5 秒かかっても、SW2 を足した 1 秒後に「SW1 + SW2 の長押し」が成立する。

数え直すのは、そうしないと**片方を長く押しているところへもう片方が滑り込んだ瞬間に
`storage host` が誤爆する**ため（SW1 を単独で押し続けているところへ、しきい値の
0.05 秒前に SW2 が入ると、その 0.05 秒後には両方が押されている状態で 1 秒に
達してしまう）。副作用として、SW1 を 0.9 秒押してから SW2 を足すと
SW1 単独の長押しは成立せず、1.9 秒で同時長押しになる。

離すときのずれは気にしなくてよい。**全部離すまでが 1 回の操作**なので、
SW1 を離してから SW2 を離すまでに間があっても 2 回には分かれない。

#### 制約

- **コマンドの実行中はボタンの操作が遅れる。** 押下の検出（LED の合図）はその場で出るが、
  実行はメインループがコマンドを抜けてから。`d 60000` の待機中なら最大 60 秒、
  `vgm list` / `mdx list` / `autoplay list` の長い出力中ならその終わりまで遅れる。
  古くなったからといって捨てることはしない（押したのに何も起きない方が困るため）。
- **消化しきれていない操作があるうちは、次の操作を捨てる。** 溜めて順に流すと、
  待たされた末に曲が何曲も飛ぶことになるため。
- **`storage format` の実行中の操作は失われる。** 数十秒メインループごと止まるので、
  押したことも離したことも検出されない（遅れるのではなく消える）。

#### 無効化

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 -DBUTTON_ENABLED=0
```

状態機械がリンクされず、**GP21 / GP22 は初期化もされない**ので他の用途へ回せる。
`i` の表示は `# button  : disabled` の 1 行になる。SW3（RUN）はハードウェアの配線
なので、この設定に関わらずリセットがかかる。

なお**ボタンを実装していない基板でも既定の `BUTTON_ENABLED=1` のままでよい**。
内部プルアップで「離されている」と読めるので、従来と同じ挙動になる。

### 3.21 `r`（読み出し）

/RD を叩いて OPM から 1 バイト読み出す。引数は A0 に出すレベルで、10 進の `0` か `1`。

```
> r 1
# read    : a0=1 data=0x00
OK
> w 10 20      # CLKA
OK
> w 14 05      # LOAD A + IRQEN A でタイマ A を走らせる
OK
> r 1
# read    : a0=1 data=0x01      ← タイマ A のフラグ
OK
> w 14 10      # F-Reset A（LOAD A は落とす）
OK
> r 1
# read    : a0=1 data=0x00
OK
```

`data` は D0-D7 をそのまま 16 進で出す。ビットの意味づけはしないので、
必要ならホスト側で分解する。

- **`r 1`（A0=1）がデータシート上のステータスレジスタ。** bit7 = BUSY、bit1 = タイマ B の
  フラグ、bit0 = タイマ A のフラグ。
- **`r 0`（A0=0）の読み出しはデータシートに規定が無い。** 実測では**直前に Pico 側が
  D0-D7 へ駆動した値がそのまま返る**（`w 14 05` の直後は `0x05`、`w 14 10` の直後は
  `0x10`、MDX 再生中はシーケンサが最後に書いた値）。OPM は A0=0 では D0-D7 を駆動して
  おらず、開放されたバスに残った電荷を読んでいるものと考えられる。**値に意味は無い。**

読み出しの間だけデータバスの向きが入れ替わる。OPM に駆動させる前に Pico 側を入力へ倒し、
/RD を H に戻して OPM がバスを手放してから出力へ戻すので、バスは衝突しない
（[§1.3](#13-配線上の注意)、[docs §3](docs/pico-opm-writer.md#3-opm-バスシーケンス)）。
1 回の読み出しに掛かる時間は約 2µs。

**レジスタを変えないので、再生中でも拒否しない**（[§3.19](#319-状態による拒否の一覧)）。
VGM / MDX を鳴らしたままステータスを覗ける。

**`w` の BUSY 待ちには使っていない。** `opm_write()` はデータサイクル後に固定時間
（`OPM_T_DATA_US` = 25µs）待つ方式のままで、ステータスはポーリングしない
（[docs §3.1](docs/pico-opm-writer.md#31-タイミング定数)、
[test/opm_busy/](test/opm_busy/README.md)）。

### 3.22 曲の終わり方

VGM も MDX も、曲がどう終わるかは 1 つの状態機械で決まる。`vgm status` /
`mdx status` / `autoplay status` の `# song` 行がその状態。

```
            play                 loop 上限          fade 期限
  IDLE ───────────> PLAYING ─────────────────> FADING ─────────┐
   ▲                  │                                        │
   │                  │ 自然終了 / stop / 壊れていた             │
   │                  ▼                                        ▼
   └───── 無音 ──── RINGOUT <───────────────────────────────────┘
                      │
                  play（曲送り / 次の曲）──> PLAYING
```

余韻を待っている最中に次の曲が始まったら、待たずに `PLAYING` へ戻る
（`autoplay next` や `autoplay gap 0` がこの経路）。

| 状態 | 意味 |
| --- | --- |
| `IDLE` | 鳴っていない |
| `PLAYING` | 演奏中 |
| `FADING` | ループ回数が `loop` に達してフェードアウト中。演奏はまだ続いている |
| `RINGOUT` | 演奏は止まった。**余韻が消えるのを待っている** |

#### ループ回数で終わらせる

`vgm loop <n>` / `mdx loop <n>` を `0` 以外にすると、その回数だけ周回したところで
`fade` ミリ秒かけてフェードアウトし、演奏を止める。**既定は `0`（無限）**なので、
何も設定しなければ従来どおりループを持つ曲は止まらない。

フェードアウトは出力ゲインで作るので、**I2S 出力と USB キャプチャにしか効かない。**
YM3012 のアナログ出力はフェードが終わるまで元の音量のままで、音が消えるのは
そのあとのキーオフ（[§5](#5-i2s-出力)）。

自動再生には `autoplay loop` / `autoplay fade` という別の設定があり、自動再生中は
そちらが優先される（[§3.17](#317-autoplay自動再生)）。既定値が違う（手動再生は
`0` = 無限、自動再生は `2`）ので 2 系統に分けてある。

#### 曲の終わりの余韻

**曲データが尽きたとき、ファームウェアは OPM に何も書かない。** 最後の音は音色本来の
RR で自然に減衰する（[§9.1](#91-対応範囲)）。`RINGOUT` はその減衰が終わるのを待つ状態で、
出力が 100ms のあいだ無音のままなら `IDLE` へ移る。100ms 必要なのは、波形が 1 周期に
2 回ゼロを通るため（可聴下限の 20Hz = 50ms 周期を跨ぐ長さが要る）。

RR が小さい音色が鳴ったまま曲が終わると減衰しないので、**5 秒で打ち切って強制的に
消音する**。このとき `# warn : ringout cut at 5000 ms` を出す。

強制消音しても 10 秒までに無音にならなければ、**待つのをやめて `IDLE` へ移る**
（`# warn : ringout gave up at 10000 ms`）。`RINGOUT` に留まり続けると
`songend_is_active()` が落ちず、キャプチャの終端も曲送りも同時に止まってしまうため、
状態機械に袋小路を作らない。

`vgm stop` / `mdx stop` は状態に関わらず消音するので、余韻の途中でも即座に止められる。
次の曲を `play` したときも、その先頭で全レジスタがクリアされて切れる。

#### 誰がこの状態を見るか

- **自動再生の曲送り** — `IDLE` になってから曲間の無音（`autoplay gap`）を数え始める。
  次の曲の頭に前の曲の尾が被らない
- **演奏に連動したキャプチャ**（`p 2`）— `IDLE` になった時点で WAV を閉じる
  （[§3.10](#310-ppcm-出力)）

どちらも同じ状態を見ているので、**キャプチャの終端と曲送りの時刻は必ず一致する。**

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

1. 送信を停止する（`s` の `STATE` が `ERROR` になる）
2. コマンド側の CDC #0 へ `# ERR dma overrun` を出す
3. `p 2`（演奏連動）で始めていたなら、続けて `# capture : abort (dma overrun)` を出す。
   この経路では `# capture : done` を出せないので、**これが終端の合図になる**
4. LED をエラー表示にする（[§3.9](#39-led)）
5. 次の `p 1`（または `p 0`）を受けるまで止まったまま

`# ` 始まりの情報行として出すので、「1 コマンド 1 応答」（[§3.3](#33-応答)）は崩れない
（[§3.18](#318-非同期通知)）。発生回数は `s` の `OVERRUN` に残る。
復帰は次の `p 1` でも、`p 0` で待機へ戻してからでもよい。

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

**`reset`（/IC リセット）を実行すると必ず 1 回アンダーランする。** /IC の間は YM3012 が出力を
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
I2S とは同時に使えず、**I2S が有効な既定構成では `i` / `t` の `# piotest` と `s` の
`# PIOTEST` が `SKIP (disabled)` になる**。DAC を外して診断だけ試したいときは
`-DYM3012_LOOPBACK=1` を付ける。

## 6. ビルドと書き込み

**配布されている zip を焼くだけならビルドは要らない。[§6.6](#66-リリース版の-zip-を使う) を読むこと。**

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

### 6.6 リリース版の zip を使う

ビルド済みのファームウェアは GitHub の Releases で配っている。

https://github.com/hyano/pico-opm-writer/releases

`pico-opm-writer-<バージョン>.zip` を展開すると、同じ名前のディレクトリが 1 つできる。

| ファイル | 中身 |
| --- | --- |
| `pico-opm-writer.uf2` | ファームウェア本体。これを焼く |
| `README.md` / `docs/` | このドキュメントと内部設計書 |
| `tools/opm-writer.py` / `tools/opm-record.py` | ホスト側ツール（[§10](#10-ホスト側ツール)） |
| `VERSION.txt` | どのコミットをどのオプションでビルドしたか |
| `SHA256SUMS` | 全ファイルのチェックサム |
| `LICENSE` / `THIRD-PARTY-LICENSES.md` / `licenses/` / `external/` | ライセンス（[§12](#12-ライセンス)） |

**焼き方**（[§6.4](#64-書き込み代替経路) と同じ）。BOOTSEL を押しながら USB を挿すと
`RPI-RP2` というドライブが現れるので、そこへ `pico-opm-writer.uf2` をコピーする。
コピーが終わると Pico 2 が勝手に再起動してファームウェアが立ち上がる。

展開した中身が壊れていないかを見るには、展開先のディレクトリで:

```bash
shasum -a 256 -c SHA256SUMS     # Linux では sha256sum -c SHA256SUMS
```

`VERSION.txt` の `version` が zip 名のバージョン、`describe` が元の git のタグ名
（`release/<バージョン>`）、`firmware version` がファームウェア自身が名乗る版
（起動バナーと `i` が出すもの）、`build options` がビルド時のオプションの実効値。
**問い合わせるときはこのファイルを添えること。**

FatFs の領域はファームウェアを焼いても消えない（[§7.1](#71-領域の変え方)）。
ただし `VERSION.txt` の `FLASH_FATFS_OFFSET` と `FLASH_FATFS_SIZE` が
今使っているものと違う版へ乗り換えると、領域の位置がずれてマウントできなくなる。
その場合は `storage format` でやり直す。

#### リリース用の zip を自分で作る

```bash
ninja -C build release
```

`build/release/pico-opm-writer-<バージョン>.zip` ができる。バージョンは
`git describe --tags --always` の結果から先頭の `release/` を落としたもの。
タグが無ければ短縮コミットハッシュになる。

**版を上げるときは [CMakeLists.txt](CMakeLists.txt) の `project(... VERSION x.y.z ...)`
を直してコミットしてから、`release/x.y.z` のタグを打つ。** ファームウェアの版はここの
1 箇所だけで決まり、起動バナー・`i`・`picotool info` のすべてがこの値になる。
タグの版とずれていると `ninja -C build release` はエラーで止まるので、
「zip 名は新しいのに焼いたファームは古い版を名乗る」リリースは作れない。

同じことを GitHub Actions でもやっている（`.github/workflows/build.yml`）。
push と Pull Request では zip を artifact として上げるだけで、
`release/<バージョン>` の形式のタグを push したときだけ Releases を作って zip を添付する。


## 7. ストレージ

Raspberry Pi Pico 2 に載っている **内蔵 QSPI フラッシュ 4MiB** のうち、
ファームウェアの後ろから末尾の予約セクタまでを FAT ファイルシステムとして使う。
外付けのフラッシュや SD カードは足さないので、**GPIO は 1 本も消費しない**。

```
0x10000000  ┬ ファームウェア（約 114KiB）
            ┆  （空き 約 142KiB）
0x10040000  ┬ FatFs 領域 3836KiB
0x103FF000  ┬ 末尾予約 4KiB（RP2350-E10。§7.1 参照）
0x10400000  ┴
```

| 項目 | 値 |
| --- | --- |
| 領域 | フラッシュ先頭から 0x40000、サイズ 3836KiB（3,928,064 バイト） |
| フォーマット | FAT12 / クラスタ 4096 バイト / SFD（MBR 無し） |
| 論理セクタ | 512 バイト（FatFs と USB MSC で共通） |
| ボリュームラベル | `PICOOPM` |
| VGM の置き場所 | `/VGM/` |
| MDX の置き場所 | `/MDX/` |

クラスタ長をフラッシュの消去単位と同じ 4096 バイトに揃えてあるので、
**1 クラスタの書き換えがフラッシュの消去 1 回で済む**。

### 7.1 領域の変え方

通常は **ファームウェアに残す容量（KiB）だけ**を指定すればよい。残りは末尾の
予約セクタの手前まで自動で FatFs になる。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 \
      -DFLASH_FATFS_RESERVE_KB=512
```

| `FLASH_FATFS_RESERVE_KB` | 領域の開始 | FatFs の容量 |
| --- | --- | --- |
| 128 | 0x20000 | 3964KiB |
| 256（既定） | 0x40000 | 3836KiB |
| 512 | 0x80000 | 3580KiB |
| 1024 | 0x100000 | 3068KiB |

**どこまで広げられるか**は次の 2 つで決まる。

| 境界 | 値 | 理由 |
| --- | --- | --- |
| 開始位置の下限 | ファームウェアの終端（約 114KiB）を 4KiB 切り上げた位置 | 重なるとファームウェアを自分で壊す |
| 終端の上限 | フラッシュ末尾の 1 セクタ手前（0x3FF000） | 下記の RP2350-E10 |

末尾 1 セクタを空けてあるのは、**picotool が UF2 へ RP2350-E10 エラッタ回避用の
「absolute block」を入れる**ため。これはフラッシュではなく XIP 空間の末尾
0x10FFFF00 を狙うブロックで、4MiB のフラッシュではアドレスが折り返して
末尾セクタ（0x3FF000–0x3FFFFF）に落ちる。ここを FatFs に使ってしまうと、
**BOOTSEL や `picotool load` で UF2 を焼くたびにファイルシステムの末尾 4KiB が消える**。
SWD で `.elf` を焼く場合は absolute block が付かないので起きない。

予約量を変えたいときは `-DFLASH_FATFS_TAIL_RESERVE=` で指定する（4096 の倍数）。
UF2 を一切使わない運用なら 0 にできるが、推奨しない。

オフセットとサイズをバイトで名指しすることもできる。`FLASH_FATFS_OFFSET` と
`FLASH_FATFS_RESERVE_KB` の同時指定はエラーになる。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 \
      -DFLASH_FATFS_OFFSET=3145728 -DFLASH_FATFS_SIZE=1044480
```

**領域を変えたらフォーマットし直すこと。** ジオメトリが変わるので、前の領域で
作ったファイルシステムはそのままでは使えない（§7.3）。

検査は 3 段構えになっている。

1. **コンパイル時** — 境界の 4096 バイト揃え、末尾予約への食い込み、FAT12 の
   クラスタ数上限を [flash_disk.c](src/flash_disk.c) の `_Static_assert` で見る。
2. **リンク後** — `__flash_binary_end` を読んでファームウェアと領域が重なって
   いないかを見る。重なっていればビルドが失敗する。余裕は毎回表示される。

   ```
   -- flash: firmware 116308 B (end 0x1001c654)
   --        FatFs  0x40000 + 3928064 B (end 0x3ff000)
   --        margin 145836 B  OK
   ```

3. **起動時** — 同じ `__flash_binary_end` と突き合わせる。重なっていたらマウントも
   書き込みも一切せずに `storage status` へ表示する（ビルド時に捕まえ損ねた場合の
   最後の砦。`hard_assert` で止めると復旧しづらいのでこうしてある）。

### 7.2 PC から曲データをコピーする

```
> storage host
# storage : HOST
OK
```

**SW1 と SW2 の同時長押し、または両方を押しながらの起動でも同じ状態に入れる**
（[§3.20](#320-ボタンgp21--gp22)）。PC を繋ぐ前にファイルシステムを渡しておけるので、
初めて曲を入れるときはこちらが早い。HOST 中は LED が 200ms ON / 800ms OFF になる。

PC にリムーバブルディスク `PICOOPM` が現れるので、`/VGM/` へ `.vgm` / `.vgz` を、
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
# warn    : host did not eject; files may be incomplete
# storage : PLAYER
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

**HOST から抜ける操作をボタンに割り当てているのは長押しだけ**で、短押しは
「ファイルシステムを PC へ渡している」という 1 行を出すだけで何もしない
（[§3.20](#320-ボタンgp21--gp22)）。PC がマウントしたまま抜けると書き込み途中の
ファイルが壊れるうえ、上記のとおり macOS では挿し直すまで戻せないため。
どうしても抜けられなくなったら SW3（リセット）で起動し直す。

新品の基板では領域が全 `0xFF` なのでファイルシステムが無い。
**起動時に自動でフォーマットすることはしない**ので、最初に 1 度だけ実行する。

```
> storage format yes
# format  : takes tens of seconds; I2S will underrun meanwhile
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
| ファームウェアの再書き込み | OpenOCD の `program` も `picotool load` も ELF/UF2 の中身しか書かないので、**ファイルシステムは残る**。UF2 だけは末尾セクタへ 1 ブロック書くが、そこは領域外に予約してある（§7.1） |
| `picotool erase` / BOOTSEL の nuke UF2 | フラッシュ全体を消すので**ファイルシステムも消える** |
| 容量 | 3836KiB（既定）。macOS が `.fseventsd` を作るぶん少し減る（Spotlight は `/.metadata_never_index` で抑止済み） |
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
長さぶん読み飛ばす。デュアルチップのファイルは 2 個目（`0xA4`）を無視し、
`vgm play` が `# warn : dual chip file; the second chip (0xA4) is ignored` を出す。

| 項目 | 内容 |
| --- | --- |
| 対応バージョン | 1.00 以降（データ開始位置は v1.50 未満と 0 のとき 0x40 固定） |
| 処理するコマンド | `0x54`（YM2151 書き込み） / `0x61` `0x62` `0x63` `0x70`-`0x7F`（wait） / `0x80`-`0x8F`（DAC。書き込みは飛ばし wait だけ効かせる） / `0x66`（終端） / `0x67`（データブロックを飛ばす） |
| 読み飛ばすもの | 上記以外の既知コマンドを仕様上の長さぶん |
| 中断するもの | 未知のオペコード（`0x00`-`0x2F` / `0x60` / `0x65` / `0x69`-`0x6F` / `0x96`-`0x9F`）、ファイルの途中終端。`# ERR vgm bad file (...)` を出して `ERROR` へ（[§3.18](#318-非同期通知)） |
| ループ | ヘッダのループオフセットが有効ならそこへ戻って繰り返す。`vgm loop <n>` を設定していなければ `vgm stop` するまで続く（[§3.22](#322-曲の終わり方)） |
| 終端 | ループを持たないファイルが終端に達したら**レジスタには何も書かずに**止まる。最後の音は音色本来の RR で自然に減衰する（[§3.22](#322-曲の終わり方)） |
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
# clock   : file 3546895 Hz / phiM 3579545 Hz (pitch goes down)
OK
```

**ずれるのは音程と包絡線の速さだけで、テンポは正確なまま。** VGM の wait は
チップのクロックではなく 44100Hz の絶対サンプル数だからで、だから wait を
伸縮させるのは逆効果になる（テンポまで狂う）。

追従してほしくないときは `clock fixed`（[§3.16](#316-clockクロック切り替え)）。
このとき φM は動かず、食い違いは警告として出るだけになる。

**PCM を流している間（`p 1`）に別クロックの VGM を再生しようとすると、再生自体が
`ERR wrong state` になる。** キャプチャ中のクロック切り替えを拒否しているため。
先に `p 0` を打つか、`clock fixed` にする。**このとき前の曲は既に止まっている**
（`play` はファイルを開く前に止めるため）。

**`p 2` の待機中は通る**ので、別クロックの VGM も演奏連動キャプチャで録れる
（[§3.16](#316-clockクロック切り替え)）。曲の頭で φM が切り替わり、その曲の
サンプリングレートで最後まで録れる。

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

圧縮したままフラッシュに置けるので、**領域に入る曲数が数倍になる**（YM2151 の
VGM は 3〜10 倍に縮む）。`vgm list` は `.vgm` と `.vgz` を区別せず名前順に並べ、
`vgm play` はどちらも受ける。

**圧縮されているかどうかは拡張子ではなくファイル先頭のマジック（`1F 8B`）で決める。**
中身が gzip の `.vgm` も、その逆も正しく扱える。

| 項目 | 内容 |
| --- | --- |
| 展開器 | miniz の `tinfl`（[§12](#12-ライセンス)） |
| 展開バッファ | 32KiB。DEFLATE の履歴窓を兼ねる |
| ループ | 継ぎ目に停止は出ない。非圧縮の `.vgm` と同じ（下記） |
| 追加で使う RAM | 約 84KiB。RP2350 の 512KiB に対して残りは約 330KiB |
| 追加で使う ROM | 約 10KiB |
| CRC32 | 検証しない（下記） |

**ループの継ぎ目は非圧縮と変わらない。** gzip は後方シークできないので、ループ先頭を
はじめて通過する瞬間に展開器の状態を丸ごと保存しておき、2 周目以降はそこへ戻す。
戻すのはメモリのコピーだけなので、頭出しのために展開し直すことはない。

保存できないまま通過してしまった場合の保険として、先頭から展開し直す経路がある。
こちらを通ると継ぎ目で音が数百 ms 途切れ、`s` の `gz reload` が増える
（[§3.11](#311-s統計)）。**通常の再生では 0 のまま。** この経路に入るときは
警告が出る。

```
# warn    : could not save the .vgz loop point; will re-inflate from the start
```

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

無効にすると `.vgz` は `ERR bad file` になる。

```
> vgm play OUTRUN.VGZ
# hint    : .vgz (gzip) is disabled by VGM_VGZ_ENABLED=0; gunzip before transferring
ERR bad file
```

実測サイズは
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
`0xE8` は**チャンネル数だけでなく ADPCM の制御そのものを切り替える**。`0xE8` が無い曲では
音量（`v` / `@v`）もバンク（`@n`）も効かない（[§9.7](#97-adpcm-pcm8-の再生)）。
ADPCM チャンネルは**シーケンサとしては FM と同じように回している**。同期コマンド
（`0xEF` / `0xEE`）が ADPCM 側から FM 側へ飛んでくる曲があり、止めてしまうと FM が
永久に待ち続けるため。

`.mdx` を gzip したものには対応しない。MDX 本体は数 KB〜数十 KB しかないので、
`.vgz` のような圧縮の利点が小さい。

**曲データが尽きたときレジスタには何も書かない。** 参照実装 (MXDRV) の演奏終了処理は
END_FLG を落として PCM8 を止めるだけで OPM を触らないので、それに合わせてある。
最後の音は音色本来の RR で自然に減衰し、消えるのは `mdx stop` を打つか次の曲を
`play` したときになる（[§3.22](#322-曲の終わり方)）。

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
[§2](#2-クロック設定-φm)）。`clock fixed` にしていれば従わず、食い違いを警告する。

```
> mdx play GRADIUS.MDX
# mdx     : GRADIUS.MDX
...
# clock   : mdx 4000000 Hz / phiM 3579545 Hz (pitch and tempo both off)
OK
```

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

**VGM と MDX を同時に鳴らすことはできない。** ただし `play` は走っている方を止めてから
始めるので、切り替えは 1 コマンドでできる（[§3.14](#314-vgmvgm-再生)）。読み込みに
失敗したときは前の曲へは戻らず、停止状態で終わる。

再生中は `w` / `reset` / `c` と `storage host` を拒否する（理由は VGM と同じ。
[§8.4](#84-既存機能との共存)）。MDX 本体はファイルを丸ごと RAM に載せているので
アンマウントされても読めるが、**PDX は再生中もファイルを開いたまま読み続ける**
（[§9.7](#97-adpcm-pcm8-の再生)）。どちらにせよフラッシュの消去中は数十 ms 止まる
ので拒否する。

PCM キャプチャ（`p 1`）と I2S 出力は再生中も動く。`mdx play` が φM を切り替えようと
したときに PCM を流している最中だと `ERR wrong state` になり、前の曲は止まったままに
なる。`p 2` の待機中なら切り替えは通る（[§3.16](#316-clockクロック切り替え)）。

### 9.6 MXDRV との相違点

レジスタへの書き込み内容と順序は MXDRV 2.06+17 Rel.X5-S / MXDRVg V2.00b に合わせてある
（一部の機能だけ MXDRV 2.06+16 Rel.3+25。[§12](#12-ライセンス)）。違うのは 2 点。

**BUSY 待ちをしない。** MXDRV は書き込みのたびに OPM のステータスレジスタを読んで
bit7 が下りるのを待つが、本機は固定ウェイト（アドレス後 5µs / データ後 25µs、
合計約 32µs）で代える。ステータス自体は [`r 1`](#321-r読み出し) で読めるが、
書き込み経路では見ない（[test/opm_busy/](test/opm_busy/README.md)）。
X68000 実機より遅い方向なので、詰まるとすれば書き込みが間に合わない側に出る。
`s` の `MDX TICK` の `reslip` と I2S のアンダーランで検出できる。

**余韻が消えない曲を 5 秒で打ち切る。** 参照実装は演奏終了後の音を止めないので、
RR が 0 の音色が鳴ったままだと減衰せずに残り続ける。本機は 5 秒待っても無音に
ならなければ強制的に消音し、`# warn : ringout cut at 5000 ms` を出す
（[§3.22](#322-曲の終わり方)）。それでも 10 秒までに無音にならなければ待つのをやめる。
それ以外の場面での消音のタイミングは参照実装と同じ。

ADPCM については、X68000 では PCM8（江藤啓氏の ADPCM 多重再生ドライバ）が
MSM6258 を叩く。本機はその PCM8 に相当する処理をソフトウェアで持っている
（[§9.7](#97-adpcm-pcm8-の再生)）。

### 9.7 ADPCM (PCM8) の再生

MDX ファイルのヘッダには PDX（ADPCM の波形集）の名前が入っている。
`mdx play` はその名前に `.PDX` を付けたファイルを、次の順で探す。

1. **再生中の MDX と同じフォルダ** — `/MDX/BOS/BOS01.MDX` なら `/MDX/BOS/<名前>.PDX`
2. **`/MDX/` 直下** — `/MDX/<名前>.PDX`

曲ごとのフォルダに PDX を同梱する置き方と、共通の PDX を `/MDX/` 直下へまとめる置き方の
どちらでも鳴る。`/MDX/` 直下の曲では 1. と 2. が同じパスになるので 1 回しか探さない。
大小文字は区別しないので、`THEXDER` という名前で `thexder.pdx` も見つかる。
どちらにも無ければエラーにはならず、FM パートだけがそのまま鳴る。

ヘッダの PDX 名そのものにフォルダは書けない。`/` `\` `:` と制御文字を含む名前は
（ファイルの中身は信用しないので）その場で弾く。

```
> mdx play BOS14.MDX
# mdx     : BOS14.MDX
# title   : Ｂlast Ｐower ！ ～ from BOSCONIAN-X68
# ch      : 9  voices 8
# pdx     : bos
# pdxpath : /MDX/bos.PDX
OK

> mdx play BOS/BOS01.MDX
# mdx     : BOS/BOS01.MDX
# title   : Ｇood Ｍorning ～ from BOSCONIAN-X68
# ch      : 9  voices 8
# pdx     : bos
# pdxpath : /MDX/BOS/bos.PDX
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

#### `0xE8` があるかどうかで鳴り方が変わる

X68000 では、PCM8 を使うのは **`0xE8`（PCM8 モード宣言）を出した曲だけ**。出さない曲は
IOCS の ADPCM コールへ流れる。IOCS には音量もバンクも無いので、**同じ PDX を使っても
鳴り方が違う**。本機もこの分岐をそのまま持っている。

| | `0xE8` あり | `0xE8` なし |
| --- | --- | --- |
| 発音チャンネル | 8 | 1 |
| 音量 `v` / `@v` | 効く | **効かない**（原音量 8 固定） |
| フェードアウト | 音量が下がる | 途中から**音が止まる**（定位 0） |
| バンク `@n` | 効く | **効かない**（常にバンク 0） |
| 波形の長さ | 24bit | **16bit** |

周波数（`@ED`）と定位（`p`）はどちらでも同じように効く。`0xE8` を出さない曲が ADPCM
パートに `v` を書いていても、実機と同じく無視される。

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

### 9.8 無効化

MDX 再生と ADPCM は CMake のキャッシュ変数で個別に落とせる。

```bash
# MDX 再生ごと落とす（シーケンサと 64KiB のファイルバッファがリンクされない）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 -DMDX_ENABLED=0

# FM だけ鳴らす（ADPCM のデコーダとミックスリング 約 27KiB がリンクされない）
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 -DPCM8_ENABLED=0
```

`PCM8_ENABLED` の既定は空で、`MDX_ENABLED` に従う。`-DMDX_ENABLED=0` にすれば
ADPCM も一緒に落ちるので、両方を指定する必要は無い。

`PCM8_ENABLED=0` でビルドすると `i` の `# adpcm` 行が `disabled` になり、
PDX を要求する曲は FM パートだけが鳴る。

```
# adpcm   : disabled  8 ch  pdx stream 1024 bytes/ch
...
# pdx     : THEXDER
# hint    : cannot open /MDX/THEXDER.PDX (disabled); the ADPCM part will not sound
```

`mdx pcm off`（[§3.15](#315-mdxmdx-再生)）との違いは、**コードごと消えること**。
実行時に `mdx pcm on` へ戻すことはできない。聴き比べたいだけなら `mdx pcm off` を使う。

**無効化した機能は `ERR unsupported` を返す**（`ERR wrong state` ではない。
理由は `# hint` 行に出る）。状態表示だけは通るので、無効化されていることを
`mdx` / `mdx pcm` / `i` / `t` から確認できる。

```
（PCM8_ENABLED=0）
> mdx pcm on
# hint    : ADPCM is disabled at build time (PCM8_ENABLED=0)
ERR unsupported

（MDX_ENABLED=0）
> mdx list
# hint    : MDX playback is disabled at build time (MDX_ENABLED=0)
ERR unsupported

> mdx
# mdx     : DISABLED
# pos     : 0 clocks  loop 0  ch 0
# tick    : @t 0  0 us  reslip 0
OK
```

## 10. ホスト側ツール

`tools/` にホスト PC 側のスクリプトを置いている。いずれも Python 3 の標準ライブラリだけで
動く（`.zst` を扱う機能のみ Python 3.14 以上が必要）。

| スクリプト | 役割 | ドキュメント |
| --- | --- | --- |
| `tools/opm-writer.py` | シーケンスファイルを USB CDC 経由でファームへ流し込む。行末コメント / `@KEY@` 置換 / `!capture` `!capture-song` によるキャプチャに対応。どちらもファームの CDC #1 から PCM を直接読む | [docs/opm-writer.md](docs/opm-writer.md) |
| `tools/opm-record.py` | 実機に入っている曲を 1 曲 1 ファイルで WAV に録る。曲名の指定・`--list` による一覧・`--all` による全曲録音 | [docs/opm-record.md](docs/opm-record.md) |
| `tools/opm-lfo-period.py` | 実機キャプチャから LFO の更新周期をサンプル数で測る（`--mode am` / `--mode pm`）。結果は 1 ファイル 1 行の TSV | [docs/opm-lfo-period.md](docs/opm-lfo-period.md) |
| `tools/opm-lfo-period-testgen.py` | `opm-lfo-period.py` の回帰テスト（実機不要） | [docs/opm-lfo-period-testgen.md](docs/opm-lfo-period-testgen.md) |

**キャプチャはファームの CDC #1 から PCM を直接読むので、ロジックアナライザは要らない。**
`test/` の掃引スクリプトはそのまま実行できる。

```bash
./test/dac_lr/capture_all.py --analyze
```

曲をまるごと録るなら `opm-record.py` が短い。長さを指定する必要はない。

```bash
./tools/opm-record.py --all --loop 2 -o out/
```

`test/` 以下は、これらを使った実機調査の一次データ生成環境。掃引スクリプトと測定条件は
それぞれの README にまとめてある。

| ディレクトリ | 調べていること |
| --- | --- |
| [test/lfo_noise/](test/lfo_noise/README.md) | LFO ノイズ波形（`LFRQ` / `NFRQ` の掃引） |
| [test/noise_period/](test/noise_period/README.md) | ノイズ発生器そのもの（NE でノイズを直接 DAC へ出す） |
| [test/dac_lr/](test/dac_lr/README.md) | DAC の 2 スロット (CH1/CH2) の関係 |
| [test/pcm8_sync/](test/pcm8_sync/README.md) | FM と ADPCM の発音タイミング |
| [test/opm_busy/](test/opm_busy/README.md) | BUSY フラグと `opm_write()` の待ち時間 |

## 11. 将来の拡張（本仕様の範囲外）

- **PCM8 のチェイン出力（アレイチェイン / リンクチェイン）。** MDX からは使われない
  ので後回しにしている（[§9.7](#97-adpcm-pcm8-の再生)）
- バイナリストリーミングモード（`w` の 1 行あたりのオーバーヘッド削減）
- VGM の GD3 タグ（曲名・作者）の表示
- ロータリーエンコーダ / 表示器による操作（ボタンは
  [§3.20](#320-ボタンgp21--gp22) で実装済み。曲名の表示先が無いので、
  いま鳴っている曲は CDC #0 でしか分からない）
- 自動再生の設定（曲順・ループ回数・フェード）のフラッシュへの保存。現在は電源を
  入れ直すと既定値に戻る
- `opm_write()` の固定ウェイトの短縮。`t_ADDR`（5µs）と `t_WR`（1µs）は仮置きの値で、
  最小値を測っていない（[test/opm_busy/](test/opm_busy/README.md)）
- `/IRQ` の割り込み処理。現在はレベルを参照できるだけ（`s` の `IRQ`）
- バス書き込みの PIO 化と FIFO による非同期キューイング
- 2 個目の OPM / 他の Yamaha 音源チップ (OPN 系) への対応

## 12. ライセンス

MIT License。詳細は [LICENSE](LICENSE) を参照。

ただし `pico_sdk_import.cmake` は Raspberry Pi (Trading) Ltd. 由来のファイルで、
BSD-3-Clause が適用される（ファイル先頭の表示のとおり）。

`external/` に置いた外部ソースはそれぞれの条件による（[external/README.md](external/README.md)）。

MDX の解釈（[§9](#9-mdx-再生)）は X68000 の音源ドライバ MXDRV
（(c)1988-92 milk., K.MAEKAWA, Missy.M, Yatsube）の仕様に準拠している。
準拠先は大半が **X68k MXDRV music driver version 2.06+17 Rel.X5-S / for Win32 [MXDRVg] V2.00b**
で、一部の機能だけ **MXDRV 2.06+16 Rel.3+25** に準拠する。
MXDRV のソースは本リポジトリには含まれておらず、`mdx.c` は独自に書き起こしたもの。

ADPCM パート（[§9.7](#97-adpcm-pcm8-の再生)）は **PCM8 version 0.48**
（(c) 江藤啓 1991,92）の技術資料に準拠している。PCM8 のソースやバイナリは本リポジトリ
には含まれておらず、`pcm8.c` は独自に書き起こしたもの。

| 置き場所 | ライセンス |
| --- | --- |
| `external/fatfs/` — FatFs R0.16（ChaN） | 1 条項の BSD 風。[external/fatfs/LICENSE.txt](external/fatfs/LICENSE.txt) |
| `external/miniz/` — miniz 3.1.2 | MIT（本プロジェクトと同一）。[external/miniz/LICENSE](external/miniz/LICENSE) |
