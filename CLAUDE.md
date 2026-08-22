# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## プロジェクト概要

Raspberry Pi Pico 2 (RP2350 / `PICO_BOARD=pico2`) から YM2151 (OPM) 音源チップのレジスタを書き込むためのファームウェア。

使い方・配線・コマンド仕様は [README.md](README.md) に、ファームウェアの内部設計と実装は [docs/pico-opm-writer.md](docs/pico-opm-writer.md) に記載されている。ファームウェアの主要ファイル：

| ファイル | 役割 |
| --- | --- |
| `pico-opm-writer.c` | main・コマンドパーサ |
| `opm.c` / `opm.h` | YM2151 バス制御（GPIO / PIO） |
| `opm_clock.pio` | φM 生成（PIO） |
| `clockmode.c` / `clockmode.h` | φM プリセットの実行時切り替え（sys_clk と PIO 分周比の張り替え順序） |
| `ym3012.c` / `ym3012.h` / `ym3012.pio` | YM3012 DAC キャプチャ / DMA リング / PCM 変換 |
| `capture.c` / `capture.h` | キャプチャ状態機械 |
| `i2s.c` / `i2s.h` / `i2s.pio` | I2S 出力（PCM5102A、GP26-GP28） |
| `usb_pcm.c` / `usb_pcm.h` | CDC #1 PCM 出力 |
| `flash_disk.c` / `flash_disk.h` | 内蔵フラッシュ後半のブロックデバイス（領域定数・ライトバックキャッシュ） |
| `ffconf.h` / `diskio_flash.c` | FatFs の設定と disk I/O 実装 |
| `storage.c` / `storage.h` | ストレージのモード状態機械 / マウント / フォーマット |
| `filelist.c` / `filelist.h` | FatFs 上のファイル一覧。出力（`vgm list` / `mdx list`）と、RAM へ集める `filelist_collect()`（autoplay 用）。**この 2 本は `FILINFO` を共用していて再入できない** |
| `usb_msc.c` | USB マスストレージの `tud_msc_*` コールバック |
| `vgm.c` / `vgm.h` | VGM の解析・再生・一覧 |
| `vgz.c` / `vgz.h` | `.vgz`（gzip）のストリーム展開。一時ファイルは作らない |
| `mdx.c` / `mdx.h` | MDX (X68000 / MXDRV) の解析・再生・一覧。解釈は **MXDRV 2.06+16 Rel.3+25 の仕様に準拠**（ソースは同梱していない） |
| `pcm8.c` / `pcm8.h` | MDX の ADPCM パート。PDX を FatFs からストリーミングし、MSM6258 の ADPCM をソフトウェアでデコードして FM の PCM に加算する。解釈は **PCM8 (江藤啓) v0.48 の技術資料に準拠**（資料・ソース・バイナリとも同梱していない）。出力レートは ADPCM レートの整数倍になるので**補間しない** |
| `autoplay.c` / `autoplay.h` | VGM / MDX の自動連続再生。プレイリスト・曲順・曲送りの状態機械。フェードアウトは `ym3012_fade_start()` の出力ゲインで作るので **I2S と USB キャプチャにしか効かない**（YM3012 のアナログ出力は素通り） |
| `led.c` / `led.h` | LED 表示 |
| `stats.c` / `stats.h` | 実行時統計 |
| `tusb_config.h` / `usb_descriptors.c` | USB CDC 2 本 + MSC 1 本 |
| `external/fatfs/` | FatFs R0.16（**上流のまま。改変しない**。出所と適用パッチは `external/README.md`） |
| `external/miniz/` | miniz 3.1.2（**上流のまま。改変しない**。展開器 `tinfl` だけを使う。設定は `CMakeLists.txt` の `miniz` ターゲットの `MINIZ_NO_*`） |

これとは別に、ホスト PC 側の Python スクリプトが `tools/` に 3 本ある。リファレンスは `docs/` にあり、
ファイル名は拡張子を落とした `docs/<スクリプト名>.md`（`docs/` にはこれらに加えて上記の
`docs/pico-opm-writer.md` が入る）。

| スクリプト | 役割 | ドキュメント |
| --- | --- | --- |
| `tools/opm-writer.py` | シーケンスファイルを USB CDC 経由でファームへ流し込む。`!capture` でファーム側の CDC #1 から PCM をキャプチャ | [docs/opm-writer.md](docs/opm-writer.md) |
| `tools/opm-lfo-period.py` | キャプチャから LFO の更新周期をサンプル数で測る（`--mode` で AM/PM を指定）。出力は TSV | [docs/opm-lfo-period.md](docs/opm-lfo-period.md) |
| `tools/opm-lfo-period-testgen.py` | `opm-lfo-period.py` の回帰テスト（実機不要） | [docs/opm-lfo-period-testgen.md](docs/opm-lfo-period-testgen.md) |

`test/` 以下は上記を使った実機調査の一次データ生成環境。

| ディレクトリ | 内容 |
| --- | --- |
| [test/lfo_noise/](test/lfo_noise/README.md) | LFO ノイズ波形の調査（`LFRQ` / `NFRQ` の掃引）。掃引・解析とも完了済みで、結論と根拠は README §1〜§3、実測値は `result/`。`wav_w1/`（矩形）は**意図的にスコープ外**で、一次データを残すだけで解析しない（README §4.11） |
| [test/noise_period/](test/noise_period/README.md) | ノイズ発生器そのものの調査。NE (`0x0F` bit7) でノイズを ch7 の C2 に直接出すと **DAC 出力が 2 値**になり、符号列がノイズ発生器のビット列そのものになる |
| [test/dac_lr/](test/dac_lr/README.md) | DAC の 2 スロット (CH1/CH2) の関係。**L と R は同一にならない**ので、波形解析には片側だけを使う |

## 開発上の約束

- ユーザーとのやり取り、コミットメッセージ、コード中のコメント、ドキュメントはすべて
  **日本語** で記述する。
- **ファームウェアの実行時応答（`printf` / `puts` で CDC #0 へ出す文字列）だけは英語。**
  `# hint` / `# warn` / `# ERR` も含めて例外なく英語で書く（README §3.3）。日本語で
  よいのは `_Static_assert` のメッセージ（開発者向けのコンパイル時診断）だけ。
  `mdx play` の `# title` 行は MDX ファイル中の Shift_JIS をそのまま流している。
- ホスト側スクリプトのリファレンスは `docs/<スクリプト名>.md` に置く（`.py` は付けない）。
- ドキュメントには仕様変更の経緯を書かない。常に最新の状態だけを記述する。

## 技術解析レポートの構成

対象は `test/*/README.md`（実機調査のレポート）。`docs/*.md` はスクリプトのリファレンス
なのでこの規約の対象外。

**結論を先に、根拠を次に、詳細を後に置く。** 解析を実施した順番とレポートを読む順番は
一致させない。試行錯誤の時系列ではなく、読者が理解しやすい順に再構成する。

章立ては原則としてこの順。**内容に無い章は作らない**し、名称は内容に合わせて変えてよい。
重要なのは章名ではなく結論を先に出す構造。

```
## 解析結果の要約
## 判明した仕様
## 結論の根拠
## 解析方法
## 詳細な解析結果
## 考察
## 不明点・未解決事項
## 今後の課題
```

- **冒頭の「解析結果の要約」は必須。** `| 項目 | 結論 | 根拠 |` の表にして、
  各行に根拠の節へのリンクを張る。表より箇条書きが分かりやすいときは箇条書きでよい。
  **確度の列は作らない**（載せるのは確定した結論だけなので、全行が同じ値になる）。
  項目の欄は**何と何の関係を書いた行なのかが分かる名前**にする。レジスタ名や
  「更新周期」だけの 1 語で済ませず、「`LFRQ` 上位ニブルと更新周期の関係」のように書く。
- **要約に載せるのは確定した結論だけ。** 確度が「有力」以下のもの、不明なこと、
  測定条件はいずれも書かない。まだ確定していない事項は「詳細な解析結果」と
  「不明点・未解決事項」へ、どの設定で測ったかは「解析方法」へ回す。
  交絡が残っていて確定と言い切れないものは、根拠の節では扱っても要約には出さない。
  **調査対象そのものの仕様でないものも載せない。** デコーダや解析器が正しく動いている
  ことの確認は、結論を支える前提であって YM2151 の仕様ではないので、根拠の節に置く。
  **他の行から導かれるだけの帰結も 1 行に立てない。**（根拠の欄が「上の N 行の帰結」に
  なる行は、それ自体が独立した結論ではないので落とす。帰結は「考察」で述べる。）
  例外は**結論の値そのものが観測範囲で決まっている**場合だけで、このときは
  「観測できた下限は〜」のように値の側に書く（別行の但し書きにはしない）。
- **確度は本文の言い回しで書き分ける。** 実測は根拠の置き場を併記、推測は
  「〜と考えられる」、未確認は「未確認」。確度が低い内容を断定しない。
  確定していない事項が要約に上がらない以上、レポート中に確度のタグや列は要らない。
- **観測した現象と、そこから推定した内部実装を分ける。** ハードウェアの内部動作を
  推定する節では「観測した現象」「そこから言えること」「そこからは言えないこと」を
  はっきり分けて書く。
- **詳細は削らず後半に置く。** 実験条件・レジスタ設定・測定値・計算式・比較結果・
  再現実験・失敗した実験・否定された仮説・判断に至った理由はすべて残す。
  「短時間で読める」は「短く書く」ことではない。
- **掃引スクリプトの操作手順・オプション表は末尾の付録へ。** 結論に到達する前に
  読ませない。
- **「不明点・未解決事項」も成果**として書く。何が分からないのか / なぜ分からないのか /
  どこまで絞り込めているのか / どの仮説が残っているのかを明記する。
- **「今後の課題」は「未解決事項 → 理由 → 次に行う実験」まで**書く。「追加調査が必要」で
  止めない。何をどう変えて何を測れば判別できるかまで踏み込む。
- **同じ結論を繰り返さない。** 要約は結論を短く / 根拠はなぜそう判断したか / 詳細は
  実験とデータ / 考察はそこから何が言えるか / 未解決事項は何がまだ分からないか、と
  役割を分ける。表や式は 1 箇所に置き、他の節からは参照する。
- **読者が 3 段階で情報を取れること**を目標にする。30 秒（冒頭だけで何を調査し何が
  判明したか）/ 2 分（要約と根拠でなぜその結論か・どこまで確定か・何が未解決か）/
  詳細（本文全部で実験条件・測定方法・データ・解析方法・再現方法まで追える）。
- **更新するときは末尾に足すだけにしない。** 新しい実験結果を入れたら、冒頭の
  「解析結果の要約」と「判明した仕様」も合わせて直す。
- 新しい解析を始めるときも最初からこの構成を意識し、得られた情報を観測事実 / 根拠 /
  推定 / 仮説 / 確定した仕様 / 未解決事項に分類しながら進める。解析が一段落したら、
  「最終的に何が判明したのか」を先に整理してから詳細を書く。

章を組み替えると `§` 参照とアンカーリンクがずれる。`docs/*.md` や `test/*/(スクリプト).py`
の docstring からも節を指しているので、`grep -rn --include='*.md' --include='*.py' '§' .`
で残りを確認する。

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

主なキャッシュ変数（いずれも `-D<名前>=<値>` で再コンフィグして切り替える）:

| 変数 | 既定 | 効果 |
| --- | --- | --- |
| `OPM_CLOCK_MODE` | 空（`opm.h` の既定） | φM プリセット（0 = 4MHz / 1 = 3.579545MHz） |
| `I2S_ENABLED` | 1 | I2S 出力（GP26-GP28） |
| `VGM_VGZ_ENABLED` | 1 | `.vgz`（gzip）の再生。0 にすると展開器と約 86KB のバッファがリンクされず、`.vgz` は `bad file` になる |
| `MDX_ENABLED` | 1 | MDX の再生。0 にするとシーケンサと 64KB のファイルバッファがリンクされない |
| `PCM8_ENABLED` | 空（`MDX_ENABLED` に従う） | MDX の ADPCM パート。0 にするとデコーダとミックスリング（約 27KB）がリンクされず、ADPCM は鳴らない |
| `AUTOPLAY_ENABLED` | 1 | 自動連続再生。0 にするとプレイリスト（約 14KB）と状態機械がリンクされず、`autoplay` は `unsupported` になる |
| `FLASH_FATFS_OFFSET` / `FLASH_FATFS_SIZE` | 空（`flash_disk.h` の既定） | FatFs 領域 |
| `YM3012_LOOPBACK` | 空（`I2S_ENABLED` から自動） | 起動時ループバック自己診断 |
| `STATS_PROFILE` | 0 | サービスごとの滞在時間の計測。1 にすると `s` に `SVCTIME` 行（µs/s）が増える。区間ごとに時刻を 2 回読むので常用しない |

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

代替経路（BOOTSEL 経由）。BOOTSEL を押しながら USB を挿してから実行する:

```bash
picotool load build/pico-opm-writer.uf2
```

または `build/pico-opm-writer.uf2` を BOOTSEL 起動時の RPI-RP2 ドライブへコピーする。

CDC を 2 本にするため TinyUSB のディスクリプタを自前で持っており、`PICO_ENABLE_USB_RESET_VIA_VENDOR_INTERFACE` が無効になるので **`-fx`（BOOTSEL 操作なしの書き込み）は使えない**。

GUI でのステップ実行デバッグは VS Code の "Pico Debug (Cortex-Debug)" 構成を使う。

### シリアル (stdio) の読み取り

| デバイス | 中身 |
| --- | --- |
| `/dev/cu.usbmodem112101` | **ターゲット pico2 自身の USB CDC #0** = コマンド / `printf` の出力先 |
| `/dev/cu.usbmodem112103` | **ターゲット pico2 自身の USB CDC #1** = PCM データ出力（signed 16bit LE ステレオ、サンプリングレート φM/64） |
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

**ファームウェア自身が持つ自己診断**：
- `t` コマンド — PCM 変換、MDX の音程 / テンポ換算、ADPCM デコーダの既知ベクタ検証と、起動時の PIO ループバック診断結果を表示。**ループバック診断は GP26-GP28 を使うので、I2S が有効な既定構成では `SKIP (disabled)` になる**（`-DYM3012_LOOPBACK=1` で強制できるが DAC は外すこと）
- `s` コマンド — 実行時統計を表示（サービス関数に居た時間の割合と `tud_task()` の占有率 / DMA リング使用量と high-water / USB TX 滞留量 / I2S の先行量と low-water / DMA overrun 回数 / I2S アンダーラン回数 / 禁止コード E=0 の数 / 実測フレームレート / フラッシュ書き出し回数と停止時間 / VGM の再生位置と遅れ / `.vgz` を先頭から展開し直した回数 / MDX の再生位置・テンポ・遅れ / ADPCM の発音チャンネル数と PDX 読み出し回数と飽和数 / メインループ周回数）
- `storage status` コマンド — ストレージのモード・領域・ファイルシステム・キャッシュの状態
- `autoplay status` / `autoplay list` コマンド — 自動再生の状態とプレイリスト
- `s 0` — 統計をリセット

**ホスト側スクリプトの検証**は次の 4 本。いずれも実機は要らず、全ケース `PASS` で終了コード 0:

```bash
./tools/opm-lfo-period-testgen.py          # opm-lfo-period.py の回帰テスト（30 秒 / 47 ケース）
./test/dac_lr/lr_relation.py --self-test   # L/R 判定器の自己検証（1 秒 / 16 ケース）
./test/lfo_noise/analyze_lfo.py --self-test # 段ごとの LFO 値抽出・値列の突き合わせ・段の間隔の自己検証（15 秒 / 46 ケース）
./test/noise_period/analyze_noise.py --self-test # ノイズ発生器の周期推定の自己検証（1 秒 / 14 ケース）
```

**実機調査**：`tools/opm-writer.py` は `test/` の掃引スクリプトをそのまま実行できる。
ファーム側の CDC #1 から PCM を直接読む。ファーム側の DMA リングは
16KB（62500 フレーム/s で 65.5ms 分）しかないので、取り込み中はホストが読み続ける必要がある。

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

`target_link_libraries` に `hardware_*` を追加する（現状は `pico_stdlib` + `hardware_pio` + `hardware_dma` + `hardware_clocks` + `hardware_flash` + `pico_flash` + `tinyusb_device` + `fatfs` + `miniz`）。

`external/` の上流コードは `add_library(... STATIC ...)` の別ターゲットにする（`fatfs` / `miniz`）。`-Wall -Wextra` が `pico-opm-writer` に `PRIVATE` で付いているので、これで上流へ波及せず警告抑止も改変も要らなくなる。

### システムクロックは φM とペア（既定 144MHz / φM 4MHz）

`opm.h` に φM と sys_clk のプリセットが 2 組あり、`OPM_CLOCK_MODE` で選ぶ（README §2）:

| プリセット | φM | sys_clk | clkdiv |
| --- | --- | --- | --- |
| `OPM_CLOCK_MODE_4MHZ`（既定） | 4.000000MHz | 144MHz | 18 |
| `OPM_CLOCK_MODE_NTSC` | 3.579545MHz | 157.5MHz | 22 |

`main()` の先頭で `set_sys_clock_khz(OPM_SYS_CLOCK_KHZ, true)` を `stdio_init_all()` より前に呼んでいる。どちらの組も `φM × 偶数 = sys_clk` がちょうど成り立つので整数分周になりジッタが乗らない。**片方だけを書き換えないこと**（小数分周になる）。SDK 既定の 150MHz に戻すとどちらのプリセットも小数分周になるので、この呼び出しも消さないこと。

ヘッダを触らずに切り替えるときは `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 -DOPM_CLOCK_MODE=1` で再コンフィグする（`-DOPM_CLOCK_MODE=` 空で `opm.h` の既定に戻る）。

クロック依存の遅延はすべて `clock_get_hz(clk_sys)` から実行時に算出しているため、周波数を変えても定数の書き換えは要らない。

I2S の分周比は `i2s_init()` が **φM の分周値（256 倍固定小数）をそのまま 2 倍**して作る（`clkdiv_i2s = sys_clk/φM = 2 × clkdiv_opm`）。丸めを挟まないので、サンプリングレートは φM/64 と厳密に一致する。**φM 側だけを変えても I2S は自動で追従する**（docs §5.2）。

## Git

`.gitignore` は `build` / `__pycache__` / `!.vscode/*` / `*.wav` / `*.bin` の 5 行。**`.vscode/` は意図的に git 管理する**（`tasks.json` の Flash / `launch.json` のデバッグ構成、`settings.json` の PATH 設定を共有するため）。`*.wav.zst` は除外していないので、`test/lfo_noise/wav/` のキャプチャ結果は git の管理対象に入る。
