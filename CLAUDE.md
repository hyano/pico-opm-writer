# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## プロジェクト概要

Raspberry Pi Pico 2 (RP2350 / `PICO_BOARD=pico2`) から YM2151 (OPM) 音源チップのレジスタを書き込むためのファームウェア。

仕様は [README.md](README.md) に全部書いてある。ファームウェアは
`pico-opm-writer.c`（行入力 + コマンドパーサ）/ `opm.h` / `opm.c`（バス制御）/ `opm_clock.pio`（φM 生成）の 4 ファイル構成。

これとは別に、ホスト PC 側の Python スクリプトが `tools/` に 5 本ある。リファレンスは `docs/` にあり、
ファイル名は拡張子を落とした `docs/<スクリプト名>.md`。

| スクリプト | 役割 | ドキュメント |
| --- | --- | --- |
| `tools/opm-writer.py` | シーケンスファイルを USB CDC 経由でファームへ流し込む（`!capture` でロジアナも制御） | [docs/opm-writer.md](docs/opm-writer.md) |
| `tools/opm-dac2wav.py` | DAC 出力の生ロジックキャプチャを 16bit ステレオ WAV へデコード | [docs/opm-dac2wav.md](docs/opm-dac2wav.md) |
| `tools/opm-dac-testgen.py` | `opm-dac2wav.py` の回帰テスト（実機不要） | [docs/opm-dac-testgen.md](docs/opm-dac-testgen.md) |
| `tools/opm-lfo-period.py` | キャプチャから LFO の更新周期をサンプル数で測る（`--mode` で AM/PM を指定）。出力は TSV | [docs/opm-lfo-period.md](docs/opm-lfo-period.md) |
| `tools/opm-lfo-period-testgen.py` | `opm-lfo-period.py` の回帰テスト（実機不要） | [docs/opm-lfo-period-testgen.md](docs/opm-lfo-period-testgen.md) |

`test/` 以下は上記を使った実機調査の一次データ生成環境。

| ディレクトリ | 内容 |
| --- | --- |
| [test/lfo_noise/](test/lfo_noise/README.md) | LFO ノイズ波形の調査（`LFRQ` / `NFRQ` の掃引）。掃引・解析とも完了済みで、結論と根拠は README §6 / §7、実測値は `result/`。`wav_value/` と `wav_w0,1,2/` だけは**一次データのみで解析は未了**（README §6.8） |
| [test/dac_lr/](test/dac_lr/README.md) | DAC の 2 スロット (CH1/CH2) の関係。**L と R は同一にならない**ので、波形解析には片側だけを使う |

## 開発上の約束

- ユーザーとのやり取り、コミットメッセージ、コード中のコメントはすべて **日本語** で記述する。
- ホスト側スクリプトのリファレンスは `docs/<スクリプト名>.md` に置く（`.py` は付けない）。
- ドキュメントには仕様変更の経緯を書かない。常に最新の状態だけを記述する。

## ビルド・書き込み

ツールチェーンは `~/.pico-sdk/` 配下にバージョン固定でインストールされている。**システムの cmake / ninja / arm-none-eabi-gcc は使わない。**

| 用途 | パス |
| --- | --- |
| cmake | `~/.pico-sdk/cmake/v4.3.4/bin/cmake` |
| ninja | `~/.pico-sdk/ninja/v1.13.2/ninja` |
| toolchain | `~/.pico-sdk/toolchain/15_2_Rel1/bin` |
| picotool | `~/.pico-sdk/picotool/2.3.0/picotool/picotool` |
| openocd | `~/.pico-sdk/openocd/0.12.0+dev` |

VS Code のターミナルでは `.vscode/settings.json` の `terminal.integrated.env.osx` がこれらを PATH と環境変数に設定するが、**CLI から直接叩く場合は自分で export する必要がある**:

```bash
export PICO_SDK_PATH=~/.pico-sdk/sdk/2.3.0
export PICO_TOOLCHAIN_PATH=~/.pico-sdk/toolchain/15_2_Rel1
export PATH=~/.pico-sdk/toolchain/15_2_Rel1/bin:~/.pico-sdk/picotool/2.3.0/picotool:~/.pico-sdk/cmake/v4.3.4/bin:~/.pico-sdk/ninja/v1.13.2:$PATH
```

### 増分ビルド（通常はこれだけで足りる）

```bash
~/.pico-sdk/ninja/v1.13.2/ninja -C build
```

`.vscode/tasks.json` の "Compile Project" と等価。`build/` は Release / `rp2350-arm-s` で構成済み。

### 再コンフィグ（`CMakeLists.txt` や `.pio` の追加時）

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2
```

### 実機への書き込み（SWD / PicoProbe が既定）

PicoProbe (Debugprobe on Pico, CMSIS-DAP, VID:PID `2e8a:000c`) がターゲット pico2 の SWD に常時接続されている。**CLI からはこれで焼くのが既定**（BOOTSEL 操作もターゲットの USB 状態も不要）:

```bash
~/.pico-sdk/openocd/0.12.0+dev/openocd \
  -s ~/.pico-sdk/openocd/0.12.0+dev/scripts \
  -f interface/cmsis-dap.cfg \
  -f target/rp2350.cfg \
  -c "adapter speed 5000; program build/pico-opm-writer.elf verify reset exit"
```

`.vscode/tasks.json` の "Flash" タスクと等価。成功時は `** Programming Finished **` / `** Verified OK **` / `** Resetting Target **` が出る。`openocd` は PATH に無いのでフルパスで叩く。DAP に繋がらなくなったら `target/rp2350-rescue.cfg` を使う "Rescue Reset" タスク相当を先に実行する。

代替経路（USB 側が生きている場合のみ）:

```bash
picotool load build/pico-opm-writer.uf2 -fx   # BOOTSEL 不要、書き込み後に実行まで行う
```

または `build/pico-opm-writer.uf2` を BOOTSEL 起動時の RPI-RP2 ドライブへコピーする。
GUI でのステップ実行デバッグは VS Code の "Pico Debug (Cortex-Debug)" 構成を使う。

### シリアル (stdio) の読み取り

| デバイス | 中身 |
| --- | --- |
| `/dev/cu.usbmodem112101` | **ターゲット pico2 自身の USB CDC** = `printf` の出力先 |
| `/dev/cu.usbmodem112202` | PicoProbe 側の CDC-UART ブリッジ（現状の設定では未使用） |

tty 名は USB のポート位置に依存する。変わったら `ioreg -r -c IOSerialBSDClient -l -w 0` の `locationID` と `ioreg -p IOUSB -l -w 0` の `USB Product Name` を突き合わせて引き直す（ターゲットは Product Name `Pico`、プローブは `Debugprobe on Pico _CMSIS_DAP_`）。

読むときの注意:

- **`/dev/tty.*` ではなく `/dev/cu.*` を使う。** `tty.*` は DCD 待ちで open がブロックする。
- この Mac には `timeout` / `gtimeout` も pyserial も**入っていない**。時間制限付きの読み取りは python3 標準ライブラリ（`os.open` の `O_NONBLOCK` + `select`）で書く。
- **SWD の `reset` でターゲットの USB CDC が切断・再列挙される。** 書き込み直後はデバイスノードが数秒消えるので、存在を待ってから開く。
- `stdio_usb` はホストが開く前の出力を捨てるため、起動直後の行は取り逃す。周期出力で確認する。

```bash
python3 - <<'EOF'
import os, select, time
dev = "/dev/cu.usbmodem112101"
for _ in range(40):                 # 書き込み後の再列挙を待つ
    if os.path.exists(dev): break
    time.sleep(0.5)
time.sleep(1.0)
fd = os.open(dev, os.O_RDONLY | os.O_NONBLOCK | os.O_NOCTTY)
buf, end = b"", time.time() + 5     # 5 秒だけ読む
while time.time() < end:
    if select.select([fd], [], [], max(0, end - time.time()))[0]:
        try: buf += os.read(fd, 4096)
        except BlockingIOError: pass
os.close(fd)
print(buf.decode("utf-8", "replace"))
EOF
```

### 成果物

`build/pico-opm-writer.{uf2,elf,bin,hex,dis}` および `build/compile_commands.json`（`CMAKE_EXPORT_COMPILE_COMMANDS ON`）。SWD 書き込みには `.elf` を使う。

### テスト

**ファームウェアの検証**は **増分ビルド → SWD で書き込み → `/dev/cu.usbmodem112101` の出力を確認** の 3 ステップで行う（上記の各節そのまま）。ホスト上で走るファームウェアのテストは存在しない。

**ホスト側スクリプトの検証**は次の 3 本。いずれも実機もロジアナも要らず、全ケース `PASS` で終了コード 0:

```bash
./tools/opm-dac-testgen.py                 # opm-dac2wav.py の回帰テスト（1 秒）
./tools/opm-lfo-period-testgen.py          # opm-lfo-period.py の回帰テスト（25 秒 / 39 ケース）
./test/dac_lr/lr_relation.py --self-test   # L/R 判定器の自己検証（1 秒 / 16 ケース）
```

出力が変わらない機能を触ったときは、**その回だけ判別できる文字列**（`__DATE__` / `__TIME__` や連番）を `printf` に一時的に混ぜると、「新しいバイナリが本当に焼けたか」を出力だけで切り分けられる。2026-08-08 にこの手順で書き込み〜確認まで一巡することを実機で確認済み。

## 構造上の要点

### `CMakeLists.txt` の「DO NOT EDIT」ブロック

`sdkVersion` / `toolchainVersion` / `picotoolVersion` の設定と `pico-vscode.cmake` の include は Pico VS Code 拡張が管理している。手で書き換えない。

### PIO のビルドフロー

`opm_clock.pio` → `pico_generate_pio_header()` → `build/opm_clock.pio.h` が生成され、C 側は `#include "opm_clock.pio.h"` で参照する。`.pio` 内の `% c-sdk { ... %}` ブロックはそのまま生成ヘッダへ展開されるため、`opm_clock_program_init()` のような初期化ヘルパはここに書く。OPM のバス制御を PIO 化する場合も同じ構造を踏襲する。

**新しい `.pio` を追加したら `CMakeLists.txt` に `pico_generate_pio_header()` の行を足して再コンフィグが必要。**

### stdio は USB CDC のみ

`pico_enable_stdio_usb 1` / `pico_enable_stdio_uart 0`。`printf` の出力先は USB シリアル。UART を OPM 制御などに転用する際もこの設定が前提になる。

### ライブラリの追加

`target_link_libraries` に `hardware_*` を追加する（現状は `pico_stdlib` + `hardware_pio` + `hardware_clocks`）。

### システムクロックは φM とペア（既定 144MHz / φM 4MHz）

`opm.h` に φM と sys_clk のプリセットが 2 組あり、`OPM_CLOCK_MODE` で選ぶ（README §2）:

| プリセット | φM | sys_clk | clkdiv |
| --- | --- | --- | --- |
| `OPM_CLOCK_MODE_4MHZ`（既定） | 4.000000MHz | 144MHz | 18 |
| `OPM_CLOCK_MODE_NTSC` | 3.579545MHz | 157.5MHz | 22 |

`main()` の先頭で `set_sys_clock_khz(OPM_SYS_CLOCK_KHZ, true)` を `stdio_init_all()` より前に呼んでいる。どちらの組も `φM × 偶数 = sys_clk` がちょうど成り立つので整数分周になりジッタが乗らない。**片方だけを書き換えないこと**（小数分周になる）。SDK 既定の 150MHz に戻すとどちらのプリセットも小数分周になるので、この呼び出しも消さないこと。

ヘッダを触らずに切り替えるときは `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 -DOPM_CLOCK_MODE=1` で再コンフィグする（`-DOPM_CLOCK_MODE=` 空で `opm.h` の既定に戻る）。

クロック依存の遅延はすべて `clock_get_hz(clk_sys)` から実行時に算出しているため、周波数を変えても定数の書き換えは要らない。

## Git

`.gitignore` は `build` / `__pycache__` / `!.vscode/*` / `*.wav` / `*.bin` の 5 行。**`.vscode/` は意図的に git 管理する**（`tasks.json` の Flash / `launch.json` のデバッグ構成、`settings.json` の PATH 設定を共有するため）。`*.wav.zst` は除外していないので、`test/lfo_noise/wav/` のキャプチャ結果は git の管理対象に入る。
