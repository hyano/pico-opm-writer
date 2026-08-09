# pico-opm-writer

Raspberry Pi Pico 2 (RP2350) を YM2151 (OPM) に直結し、ホスト PC から USB シリアル経由で
OPM のレジスタへ値を書き込むためのファームウェア。

ホストからは USB CDC (仮想 COM ポート) として見え、`screen` や `minicom` などの素の
ターミナルからテキストコマンドを打つだけでレジスタを操作できる。バッチ実行や
DAC 出力のキャプチャには [ホスト側ツール](#7-ホスト側ツール)（`tools/`）を使う。

## 1. ハードウェア構成

### 1.1 接続表

Pico 2 は OPM に対して **書き込み専用** で接続する。データバスは常に Pico 側が駆動する。

| Pico 2 GPIO | 物理ピン | OPM 側 | 方向 | 備考 |
| --- | --- | --- | --- | --- |
| GP2  | 4  | D0   | OUT | データバス |
| GP3  | 5  | D1   | OUT | |
| GP4  | 6  | D2   | OUT | |
| GP5  | 7  | D3   | OUT | |
| GP6  | 9  | D4   | OUT | |
| GP7  | 10 | D5   | OUT | |
| GP8  | 11 | D6   | OUT | |
| GP9  | 12 | D7   | OUT | D0-D7 は連続した GPIO であること（マスク書き込みのため） |
| GP10 | 14 | A0   | OUT | L=アドレスラッチ / H=データ書き込み |
| GP11 | 15 | /WR  | OUT | 書き込みストローブ（負論理） |
| GP12 | 16 | /IC  | OUT | ハードウェアリセット（負論理） |
| GP13 | 17 | φM   | OUT | マスタークロック、PIO で生成 |
| LED  | -  | -    | OUT | 基板上 LED（アクティビティ表示）。`PICO_DEFAULT_LED_PIN`、pico2 では GP25 |
| GND  | 3, 8, 13, 18 … | GND | - | OPM と共通グラウンドを取ること |

GP0 / GP1 は将来のデバッグ UART 用に未使用のまま空けてある。

ピン番号の定義は [opm.h](opm.h) の `OPM_PIN_*` にまとまっている。

### 1.2 GPIO を割り当てない OPM 端子

| OPM 端子 | 処理 | 理由 |
| --- | --- | --- |
| /CS | **GND へ配線で L 固定** | 常時セレクト。書き込みタイミングは /WR のみで作る |
| /RD | **VCC へプルアップして H 固定** | OPM がデータバスを駆動しないようにする。**必須** |
| SO / SH1 / SH2 | DAC (YM3012 等) へ | 音声出力系。本ファームウェアは関与しない。ロジックアナライザで直接拾って WAV 化する手段については [docs/opm-dac2wav.md](docs/opm-dac2wav.md) を参照 |
| CT1 / CT2 / /IRQ | 未接続で可 | 読み出し・割り込みは使わない |

### 1.3 配線上の注意

- **電源電圧差**: OPM は 5V デバイス、Pico 2 の GPIO は 3.3V。本構成は 3.3V で直接駆動する。
  [test/](test/) の実測データはすべてこの 3.3V 直結の構成で取れているので、**手元の個体では
  動作している**。ただし OPM の入力 H レベル閾値には個体差・ロット差があり、他の個体でも
  同じように動く保証は無い（未確認）。特に **φM が不安定になる可能性がある**ので、
  動作しない場合はバッファ / レベルシフタ（74HCT244 等）を φM とバス側に挿入する。
- **逆流防止**: /RD を H に固定していれば OPM は D0-D7 を駆動しないため、5V が Pico の
  GPIO に流れ込むことはない。/RD の処理を忘れないこと。
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

## 4. ホストインタフェース

### 4.1 接続

- USB CDC (仮想 COM ポート) 1 ポート。Pico SDK の `stdio_usb` を使う
  （`pico_enable_stdio_usb 1` / `pico_enable_stdio_uart 0`）。TinyUSB は直接叩かない。
- 入出力はすべて標準入出力 API 経由:
  - 出力: `printf()` / `puts()`
  - 入力: `getchar_timeout_us()` でノンブロッキングに 1 文字ずつ受け取り、行バッファに溜める
  - 接続検出: `stdio_usb_connected()`
- ボーレート・パリティ等の設定は無視される（USB CDC のため何を指定しても動作する）。
- macOS 例: `screen /dev/cu.usbmodem112101 115200`。
  **`/dev/tty.*` ではなく `/dev/cu.*` を使う**（`tty.*` は DCD 待ちで open がブロックする）。
  デバイス名の引き直し方は [§6.4](#64-シリアル-stdio-の読み取り) を参照。
- 接続を検出した時点で起動バナーを出力する（接続前の出力は捨てられるため）。

### 4.2 行フォーマット

- コマンドは **1 行 1 コマンド**。行末は `LF` / `CR` / `CRLF` のいずれでも受け付ける。
- 1 行の最大長は 255 文字（`LINE_MAX_LEN`、[pico-opm-writer.c](pico-opm-writer.c) で定義）。
  超えた場合は `ERR too long` を返し、その行を破棄する。
- コマンド文字・16 進数の大文字小文字は区別しない。
- トークン区切りは空白またはタブ（連続可）。
- 空行、および **行頭が `#` の行**はコメントとして無視し、**応答も返さない**。

**行末コメント（`w 20 c7 # ...`）には対応していない。** `#` は引数のトークンとして読まれ、
`ERR bad argument` または `ERR wrong arity` になる。行末コメント・プレースホルダ置換・
キャプチャ制御が要るシーケンスは [tools/opm-writer.py](docs/opm-writer.md) 経由で流し込む。

### 4.3 応答

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

### 4.4 コマンド一覧

| コマンド | 書式 | 説明 |
| --- | --- | --- |
| `w` | `w <addr> <data> [<addr> <data> ...]` | レジスタ書き込み。引数は 16 進。addr/data のペアを 1 行に複数並べて連続書き込みできる。**途中でエラーになった場合、そこまでの書き込みは実行済み**のまま `ERR` を返す |
| `r` | `r` | /IC によるハードウェアリセット（[§3.2](#32-リセット手順)） |
| `c` | `c` | ソフトウェアによる全レジスタクリア（[§4.5](#45-cクリアが書き込む内容)） |
| `d` | `d <ms>` | 指定ミリ秒待機。10 進、`0`-`60000`（`DELAY_MAX_MS`） |
| `i` | `i` | 情報表示（[§4.6](#46-i情報表示の出力例)） |
| `h` | `h` / `?` | コマンド一覧を表示 |

16 進引数の桁数は自由で、値が `0xff` 以下なら受理する（`w f 1` も `w 0020 00c7` も通る）。
`0xff` を超えると `ERR out of range`。

### 4.5 `c`（クリア）が書き込む内容

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

### 4.6 `i`（情報表示）の出力例

```
# pico-opm-writer 0.1.0
# sys_clk : 144000000 Hz
# phiM    : 4000000 Hz (clkdiv 18 + 0/256)
# pins    : D0-D7=GP2-GP9 A0=GP10 /WR=GP11 /IC=GP12 phiM=GP13
# timing  : t_wr=1us t_addr=5us t_data=25us
OK
```

### 4.7 起動バナー

USB CDC の接続を検出した時点で `i` と同じ内容を出力する（末尾の `OK` を含む）。

### 4.8 使用例

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

### 4.9 LED

基板上 LED (`PICO_DEFAULT_LED_PIN`、pico2 では GP25) は、レジスタ書き込みが発生するたびに
トグルする（アクティビティ表示）。

## 5. ファームウェア構成

| ファイル | 役割 |
| --- | --- |
| [pico-opm-writer.c](pico-opm-writer.c) | `main()`、システムクロック設定、`stdio_init_all()`、行入力とコマンドパーサ、応答出力 |
| [opm.h](opm.h) | ピン割り当て・タイミング定数・φM プリセット（`OPM_CLOCK_MODE`）の定義、API 宣言 |
| [opm.c](opm.c) | GPIO 初期化、`opm_write()` / `opm_reset()` / `opm_clear()` の実装 |
| [opm_clock.pio](opm_clock.pio) | φM 生成用 PIO プログラムと `opm_clock_program_init()` |

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

クロック照会の 3 関数は [§4.6](#46-i情報表示の出力例) の情報表示に使う。

### 5.1 CMakeLists.txt の構成

Pico VS Code 拡張が管理する「DO NOT EDIT」ブロック（`sdkVersion` / `toolchainVersion` /
`picotoolVersion` の設定と `pico-vscode.cmake` の include）は手で書き換えない。
それ以外は次の構成になっている。

- `add_executable` に `pico-opm-writer.c` と `opm.c`
- `pico_set_program_version` はバイナリに埋め込むメタデータ（`picotool info` が読む）。
  `i` と起動バナーが表示する版番号はこれではなく [opm.h](opm.h) の `OPM_WRITER_VERSION`
- `pico_generate_pio_header(... opm_clock.pio)` → `build/opm_clock.pio.h` を生成
- `target_link_libraries` は `pico_stdlib` / `hardware_pio` / `hardware_clocks`
- キャッシュ変数 `OPM_CLOCK_MODE` を持ち、指定時のみ同名マクロを
  `target_compile_definitions` で渡す（φM プリセットの切り替え、[§2](#2-クロック仕様-φm)）
- `pico_enable_stdio_usb 1` / `pico_enable_stdio_uart 0`

**新しい `.pio` を追加したときは `pico_generate_pio_header()` の行を足して再コンフィグする。**

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

### 6.3 書き込み（代替経路）

ターゲットの USB 側が生きている場合のみ:

```bash
picotool load build/pico-opm-writer.uf2 -fx   # BOOTSEL 不要、書き込み後に実行まで行う
```

または `build/pico-opm-writer.uf2` を BOOTSEL 起動時の RPI-RP2 ドライブへコピーする。

GUI でのステップ実行デバッグは VS Code の "Pico Debug (Cortex-Debug)" 構成を使う。

### 6.4 シリアル (stdio) の読み取り

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

## 7. ホスト側ツール

`tools/` にホスト PC 側のスクリプトを置いている。いずれも Python 3 の標準ライブラリだけで
動く（`.zst` を扱う機能のみ Python 3.14 以上が必要）。

| スクリプト | 役割 | ドキュメント |
| --- | --- | --- |
| `tools/opm-writer.py` | シーケンスファイルを USB CDC 経由でファームへ流し込む。行末コメント / `@KEY@` 置換 / `!capture` によるロジアナ制御に対応 | [docs/opm-writer.md](docs/opm-writer.md) |
| `tools/opm-dac2wav.py` | OPM の DAC 出力 (SO/SH1/SH2) の生ロジックキャプチャを 16bit ステレオ WAV へデコードする | [docs/opm-dac2wav.md](docs/opm-dac2wav.md) |
| `tools/opm-dac-testgen.py` | `opm-dac2wav.py` の回帰テスト（実機不要） | [docs/opm-dac-testgen.md](docs/opm-dac-testgen.md) |
| `tools/opm-lfo-period.py` | 実機キャプチャから LFO の更新周期をサンプル数で測る（`--mode am` / `--mode pm`）。結果は 1 ファイル 1 行の TSV | [docs/opm-lfo-period.md](docs/opm-lfo-period.md) |
| `tools/opm-lfo-period-testgen.py` | `opm-lfo-period.py` の回帰テスト（実機不要） | [docs/opm-lfo-period-testgen.md](docs/opm-lfo-period-testgen.md) |

`test/lfo_noise/` は、これらを使って YM2151 の LFO ノイズ波形を調べるための一次データ
生成環境。掃引スクリプトと測定条件は [test/lfo_noise/README.md](test/lfo_noise/README.md) にまとめてある。

## 8. 動作確認手順

新しい環境で組んだときに一通り確かめる**手順**。1. は毎回実行して回帰検査に使うもの、
2.〜5. は配線とファームを立ち上げるときの確認項目。

1. **ホスト側の自動テスト**: 実機もロジアナも要らない。いずれも全ケース `PASS` すること。

   | コマンド | 対象 | 所要 |
   | --- | --- | --- |
   | `./tools/opm-dac-testgen.py` | DAC デコーダ (`opm-dac2wav.py`) | 1 秒 |
   | `./tools/opm-lfo-period-testgen.py` | LFO 周期解析 (`opm-lfo-period.py`) | 25 秒 |
   | `./test/dac_lr/lr_relation.py --self-test` | L/R 判定器 | 1 秒 |

2. **単体（OPM 未接続）**: 書き込み後にシリアル接続し、起動バナーが出ることと `i` の表示が
   期待どおりであることを確認する。
3. **φM の確認**: GP13 をオシロ / 周波数カウンタで測定し、既定プリセットなら
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

ファームウェア自体のホスト上での自動テストは無い。検証は
**ビルド → 書き込み → シリアル出力の確認** の 3 ステップで行う。

## 9. 将来の拡張（本仕様の範囲外）

- バイナリストリーミングモード（`w` の 1 行あたりのオーバーヘッド削減）
- 時刻付きレジスタ列の一括転送とファーム側タイマによる再生（VGM 再生の下地）
- `/RD` を接続してのステータス（BUSY）ポーリングによる待ち時間の最適化
- バス書き込みの PIO 化と FIFO による非同期キューイング
- 2 個目の OPM / 他の Yamaha 音源チップ (OPN 系) への対応

## 10. ライセンス

MIT License。詳細は [LICENSE](LICENSE) を参照。

ただし `pico_sdk_import.cmake` は Raspberry Pi (Trading) Ltd. 由来のファイルで、
BSD-3-Clause が適用される（ファイル先頭の表示のとおり）。
