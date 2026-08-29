# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Raspberry Pi Pico 2 (RP2350 / `PICO_BOARD=pico2`) から YM2151 (OPM) 音源チップのレジスタを
書き込むためのファームウェア。

**このファイルには「Claude Code が判断を誤ると壊すこと」だけを置く。** 手順や仕様の
一次情報は README と `docs/` にあり、ここからはポインタで参照する。二重に書かない。

## プロジェクト概要

*事実。*

### どこに何が書いてあるか

| 資料 | 内容 |
| --- | --- |
| [README.md](README.md) | 使い方・配線・コマンド仕様・ビルドと書き込みの手順・リリース |
| [docs/pico-opm-writer.md](docs/pico-opm-writer.md) | ファームウェアの内部設計と実装（ソース構成・PIO・DMA・各機能の仕組み・ビルド構成） |
| `docs/<スクリプト名>.md` | `tools/` の各スクリプトのリファレンス |
| `test/*/README.md` | 実機調査のレポート（下記「技術解析レポートの構成」に従う） |
| [external/README.md](external/README.md) | 上流コードの出所・適用パッチ・意図的に置いていないファイル |
| `board/` | 基板・回路図（KiCad）とベースプレート（FreeCAD）。配線の前提は README §1.3 |

### ファームウェアのソース索引

`src/` 以下。**各ファイルの役割と設計の詳細は docs §1。** ここは「どれを開くか」の索引。

| ファイル | 役割 |
| --- | --- |
| `pico-opm-writer.c` | main・初期化列・コマンドパーサ・状態表示・ボタン方針 |
| `sched.c` / `.h` | メインループの協調スケジューラ（`service_all()` と各グループの呼び出し間隔） |
| `opm.c` / `.h` / `opm_clock.pio` | YM2151 バス制御と φM 生成 |
| `clockmode.c` / `.h` | φM プリセットの実行時切り替え |
| `ym3012.c` / `.h` / `.pio` | YM3012 DAC キャプチャ・DMA リング・PCM 変換・出力ゲイン |
| `capture.c` / `.h` | キャプチャ状態機械（`p 1` 即時 / `p 2` 演奏連動） |
| `i2s.c` / `.h` / `.pio` | I2S 出力（PCM5102A、GP26-GP28） |
| `usb_pcm.c` / `.h` | CDC #1 PCM 出力 |
| `flash_disk.c` / `.h` | 内蔵フラッシュ上のブロックデバイス |
| `ffconf.h` / `diskio_flash.c` | FatFs の設定と disk I/O 実装 |
| `storage.c` / `.h` | ストレージのモード状態機械・マウント・フォーマット |
| `filelist.c` / `.h` | `/VGM` `/MDX` 以下の再帰的なファイル一覧 |
| `usb_msc.c` | USB マスストレージの `tud_msc_*` コールバック |
| `vgm.c` / `.h` | VGM の解析・再生・一覧 |
| `vgz.c` / `.h` | `.vgz`（gzip）のストリーム展開 |
| `mdx.c` / `.h` | MDX (X68000 / MXDRV) の解析・再生・一覧 |
| `pcm8.c` / `.h` | MDX の ADPCM パート（PDX のストリーム読み・MSM6258 デコード） |
| `songend.c` / `.h` | 曲の終わり方（打ち切り・フェード・余韻待ち） |
| `autoplay.c` / `.h` | VGM / MDX の自動連続再生 |
| `button.c` / `.h` | GP21 (SW1) / GP22 (SW2) の取り込み |
| `led.c` / `.h` | LED 表示 |
| `stats.c` / `.h` | 実行時統計 |
| `tusb_config.h` / `usb_descriptors.c` | USB CDC 2 本 + MSC 1 本 |
| `external/fatfs/` | FatFs R0.16（上流のまま） |
| `external/miniz/` | miniz 3.1.2（上流のまま。展開器 `tinfl` だけを使う） |

**準拠先の仕様**（ソースもバイナリも同梱していない）:
`mdx.c` は **MXDRV 2.06+17 Rel.X5-S / MXDRVg V2.00b**（一部の機能のみ 2.06+16 Rel.3+25）、
`pcm8.c` は **PCM8 (江藤啓) v0.48 の技術資料**。

### ホスト側スクリプト

`tools/` に置く。リファレンスは拡張子を落とした `docs/<スクリプト名>.md`。

| スクリプト | 役割 | ドキュメント |
| --- | --- | --- |
| `opm-writer.py` | シーケンスファイルを USB CDC 経由でファームへ流し込む。`!capture <ms>` で時間指定、`!capture-song <コマンド>` で曲 1 本ぶん（頭から余韻が消えるまで）のキャプチャ | [docs/opm-writer.md](docs/opm-writer.md) |
| `opm-record.py` | 実機の曲を 1 曲 1 ファイルで WAV に録る。**一時シーケンスを作って `opm-writer.py` をサブプロセスで起動する**（Serial と WavSink を複製しない） | [docs/opm-record.md](docs/opm-record.md) |
| `opm-lfo-period.py` | キャプチャから LFO の更新周期をサンプル数で測る。出力は TSV | [docs/opm-lfo-period.md](docs/opm-lfo-period.md) |
| `opm-lfo-period-testgen.py` | `opm-lfo-period.py` の回帰テスト（実機不要） | [docs/opm-lfo-period-testgen.md](docs/opm-lfo-period-testgen.md) |

### 実機調査 (`test/`)

上記を使った一次データ生成環境。結論と根拠は各 README にある。

| ディレクトリ | 調査結果の要点 |
| --- | --- |
| [test/lfo_noise/](test/lfo_noise/README.md) | LFO ノイズ波形（`LFRQ` / `NFRQ` の掃引）。掃引・解析とも完了済み。`wav_w1/`（矩形）は**意図的にスコープ外**で、一次データを残すだけで解析しない（README §4.11） |
| [test/noise_period/](test/noise_period/README.md) | ノイズ発生器そのもの。NE (`0x0F` bit7) でノイズを ch7 の C2 に直接出すと **DAC 出力が 2 値**になり、符号列がノイズ発生器のビット列そのものになる |
| [test/dac_lr/](test/dac_lr/README.md) | DAC の 2 スロット (CH1/CH2) の関係。**L と R は同一にならない**ので、波形解析には片側だけを使う |
| [test/pcm8_sync/](test/pcm8_sync/README.md) | FM と ADPCM の発音タイミング。**ADPCM は自分でフレーム番号を選ぶので FM より早く出ていた**（実曲で最大 2.0ms）。`flush_now()` で 0 にした |
| [test/opm_busy/](test/opm_busy/README.md) | BUSY フラグと `opm_write()` の待ち時間。**BUSY は書き込みから 67 φM サイクルで落ち、レジスタにもチップの状態にも依存しない**ので、ポーリングに優位が無い |

`test/opm_busy/` と `test/pcm8_sync/` の `diag.patch` は調査に使った診断コード。
**本体には入っていない。**

## 開発上の約束

*ルール（必ず守る）。*

- ユーザーとのやり取り、コミットメッセージ、コード中のコメント、ドキュメントはすべて
  **日本語** で記述する。
- **ファームウェアの実行時応答（`printf` / `puts` で CDC #0 へ出す文字列）だけは英語。**
  `# hint` / `# warn` / `# ERR` も含めて例外なく英語で書く（README §3.3）。日本語で
  よいのは `_Static_assert` のメッセージ（開発者向けのコンパイル時診断）だけ。
  `mdx play` の `# title` 行は MDX ファイル中の Shift_JIS をそのまま流している。
- **C のコードは `.clang-format` に従う**（Allman ブレース / 4 スペース /
  `ColumnLimit 0` / `SortIncludes Never`）。
- ホスト側スクリプトのリファレンスは `docs/<スクリプト名>.md` に置く（`.py` は付けない）。
- ドキュメントには仕様変更の経緯を書かない。常に最新の状態だけを記述する。
- **`external/` の上流コードを自前の都合で書き換えない。** 設定は自前側で行う
  （`src/ffconf.h`、`CMakeLists.txt` の `miniz` ターゲットの `MINIZ_NO_*`）。
  上流の公式パッチを当てるときだけは例外で、**素の vendoring とパッチ適用は
  コミットを分ける**（手順は external/README.md）。
- **`CMakeLists.txt` の「DO NOT EDIT」ブロックを手で書き換えない。**
  `sdkVersion` / `toolchainVersion` / `picotoolVersion` と `pico-vscode.cmake` の
  include は Pico VS Code 拡張が管理している。

## 触る前に知っておく不変条件

*事実。コードから推測すると間違える設計判断。*

### モジュール間

- **`capture.c` は vgm / mdx / songend を include しない。** `capture_note_track()` で
  bool と通し番号だけ受け取る。
- **`filelist.c` の一覧出力（`vgm list` / `mdx list`）と `filelist_collect()` は
  `FILINFO` と走査バッファを共用していて再入できない**（docs §1.3）。
- **`button.c` は autoplay も storage も知らない。** `service_all()` が再入的に
  呼ばれるため、イベントの消化は `pico-opm-writer.c` の `button_dispatch()` が
  メインループのトップレベルで行う（docs §1.1）。SW3 は RUN 端子でファームからは見えない。
- **`songend.c` が外に出す観測は `songend_is_active()` の 1 つ。** autoplay の曲送りも
  `p 2` のキャプチャの終端もこれを見る（だから必ず同じ時刻で動く）。ループ上限は
  手動再生用と autoplay 用で既定値が違うため 2 系統あり、autoplay はトラックごとに
  `songend_arm()` で差し込む（docs §11）。
- **`pcm8.c` の発音状態を変える関数は `flush_now()` でミックスリングを実時刻まで
  描き切ってから変える。** これが FM との発音時刻を合わせている
  （docs §10.7、[test/pcm8_sync/](test/pcm8_sync/README.md)）。出力レートは ADPCM
  レートの整数倍になるので**補間しない**。
- **余韻の判定は `ym3012_ring_poll()` が取り込みのついでに覚えている
  「最後に音があったフレーム」を見る。**

### ビルド・クロック

- **sys_clk と φM はペア。片方だけ書き換えない**（小数分周になってジッタが乗る）。
  `main()` 冒頭の `set_sys_clock_khz(OPM_SYS_CLOCK_KHZ, true)` も消さない
  （SDK 既定の 150MHz ではどちらのプリセットも小数分周になる）。

  | プリセット | φM | sys_clk | clkdiv |
  | --- | --- | --- | --- |
  | `OPM_CLOCK_MODE_4MHZ`（既定） | 4.000000MHz | 144MHz | 18 |
  | `OPM_CLOCK_MODE_NTSC` | 3.579545MHz | 157.5MHz | 22 |

  クロック依存の遅延はすべて `clock_get_hz(clk_sys)` から実行時に算出しているので、
  周波数を変えても定数の書き換えは要らない。I2S も φM の分周値から作るため自動で
  追従する（README §2 / docs §2・§5.2）。
- **新しい `.pio` を足したら `CMakeLists.txt` に `pico_generate_pio_header()` の行を
  足して再コンフィグする**（docs §7.1）。`% c-sdk { ... %}` は生成ヘッダへ展開されるので、
  初期化ヘルパは `.pio` の中に書く。
- **フラッシュ末尾の 1 セクタ（`FLASH_FATFS_TAIL_RESERVE`）を潰すと、UF2 で焼くたびに
  ファイルシステムの末尾が壊れる**（picotool が入れる RP2350-E10 の absolute block が
  4MiB では末尾セクタへ折り返すため。README §7.1）。リンク結果が FatFs 領域と重なる
  ほうは `cmake/check_flash_region.cmake` が `POST_BUILD` で検査して落とす。
- **ファームウェアのバージョンの唯一の定義場所は `CMakeLists.txt` の
  `project(pico-opm-writer VERSION x.y.z ...)`。** ここから `pico_set_program_version`
  と `-DOPM_WRITER_VERSION` の両方が出る。版を上げるときはこの 1 行を直して
  **コミットしてから `release/x.y.z` のタグを打つ**（README §6.6）。

## 技術解析レポートの構成

*ルール（必ず守る）。対象は `test/*/README.md`（実機調査のレポート）。`docs/*.md` は
スクリプトのリファレンスなのでこの規約の対象外。*

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

## ビルド・書き込み・確認

*手順。*

**一次情報は README §6。** ツールチェーンのパス、CLI から叩くときに必要な `export`、
BOOTSEL 経由の書き込み、GUI デバッグ、tty 名の引き直し方、リリース zip の作り方は
すべてそちらにある。**システムの cmake / ninja / arm-none-eabi-gcc は使わない。**

### 常用する 2 コマンド

`ninja` も `openocd` も PATH に無いのでフルパスで叩く。

```bash
# 増分ビルド（.vscode/tasks.json の "Compile Project" と等価）
~/.pico-sdk/ninja/v1.13.2/ninja -C build

# SWD / PicoProbe で書き込み（"Flash" タスクと等価。BOOTSEL 操作は不要）
~/.pico-sdk/openocd/0.12.0+dev/openocd \
  -s ~/.pico-sdk/openocd/0.12.0+dev/scripts \
  -f interface/cmsis-dap.cfg \
  -f target/rp2350.cfg \
  -c "adapter speed 5000; program build/pico-opm-writer.elf verify reset exit"
```

書き込み成功時は `** Programming Finished **` / `** Verified OK **` /
`** Resetting Target **` が出る。DAP に繋がらなくなったら `target/rp2350-rescue.cfg`
を使う "Rescue Reset" タスク相当を先に実行する。

### ビルドオプション

再コンフィグは `CMakeLists.txt` や `.pio` を足したときに要る。

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2
```

キャッシュ変数は `OPM_CLOCK_MODE` / `I2S_ENABLED` / `VGM_VGZ_ENABLED` / `MDX_ENABLED` /
`PCM8_ENABLED` / `AUTOPLAY_ENABLED` / `BUTTON_ENABLED` / `STATS_PROFILE` /
`YM3012_LOOPBACK` / `FLASH_TOTAL_BYTES` / `FLASH_FATFS_RESERVE_KB` /
`FLASH_FATFS_TAIL_RESERVE` / `FLASH_FATFS_OFFSET` / `FLASH_FATFS_SIZE`。
**意味・既定値・「なぜ常にマクロとして渡すか」は docs §7**、宣言は `CMakeLists.txt` の
各 `set()` 直上のコメント。`-D<名前>=<値>` を付けて再コンフィグして切り替える。

**キャッシュ変数を増やしたら `cmake/release_config.cmake.in` の `REL_OPTIONS` にも足す。**

### シリアル (stdio) の読み取り

コマンドと `printf` の出力先は **`/dev/cu.usbmodem112101`**（CDC #0）。PCM は
`/dev/cu.usbmodem112103`（CDC #1）。デバイス名は USB のポート位置に依存するので、
変わったら README §6.5 の手順で引き直す。

- **`/dev/tty.*` ではなく `/dev/cu.*` を使う。** `tty.*` は DCD 待ちで open がブロックする。
- **この Mac には `timeout` / `gtimeout` も pyserial も入っていない。**
  時間制限付きの読み取りは python3 標準ライブラリで書く。
- **SWD の `reset` でターゲットの USB CDC が切断・再列挙される。** 書き込み直後は
  デバイスノードが数秒消えるので、存在を待ってから開く。
- `stdio_usb` はホストが開く前の出力を捨てるため、起動直後の行は取り逃す。
  周期出力で確認する。

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

### 検証

**ファームウェアの検証は 増分ビルド → SWD で書き込み → CDC #0 の出力を確認 の 3 ステップ
だけ。ホスト上で走るファームウェアのテストは存在しない。**

ファーム自身の自己診断は `t`（PCM 変換 / MDX の音程・テンポ換算 / ADPCM デコーダの
既知ベクタ / PIO ループバック）・`s`（実行時統計。`s 0` でリセット）・`storage status`・
`autoplay status` / `autoplay list`。出力の読み方は README §3.11-3.13 / §3.17。
**ループバック診断は GP26-GP28 を使うので、I2S が有効な既定構成では `SKIP (disabled)`
になる**（`-DYM3012_LOOPBACK=1` で強制できるが DAC は外すこと）。

**出力が変わらない機能を触ったときは、その回だけ判別できる文字列**（`__DATE__` /
`__TIME__` や連番）を `printf` に一時的に混ぜると、「新しいバイナリが本当に焼けたか」を
出力だけで切り分けられる。

**ホスト側スクリプトの検証**は次の 7 本。いずれも実機は要らず、全ケース `PASS` で
終了コード 0:

```bash
./tools/opm-record.py --self-test          # 曲指定 / 出力名 / シーケンス生成 / 一覧の解析（1 秒 / 23 ケース）
./tools/opm-lfo-period-testgen.py          # opm-lfo-period.py の回帰テスト（30 秒 / 47 ケース）
./test/dac_lr/lr_relation.py --self-test   # L/R 判定器の自己検証（1 秒 / 16 ケース）
./test/lfo_noise/analyze_lfo.py --self-test # 段ごとの LFO 値抽出・値列の突き合わせ・段の間隔の自己検証（15 秒 / 46 ケース）
./test/noise_period/analyze_noise.py --self-test # ノイズ発生器の周期推定の自己検証（1 秒 / 14 ケース）
./test/pcm8_sync/analyze_sync.py --self-test # 立ち上がり検出と対応づけの自己検証（1 秒 / 15 ケース）
./test/pcm8_sync/gen_testdata.py --self-test # 生成する MDX / PDX の書式の自己検証（1 秒 / 27 ケース）
```

**実機調査**では `tools/opm-writer.py` が `test/` の掃引スクリプトをそのまま実行できる。
ファーム側の CDC #1 から PCM を直接読む。**DMA リングは 16KB（62500 フレーム/s で
65.5ms 分）しかないので、取り込み中はホストが読み続ける必要がある。**

## Git

*事実。除外の一覧は [.gitignore](.gitignore) を読む。ここには理由だけ書く。*

- **`.vscode/` は意図的に git 管理する**（`tasks.json` の Flash、`launch.json` の
  デバッグ構成、`settings.json` の PATH 設定を共有するため）。
- `reference/` は同梱しない参照資料と試聴用データ（MXDRV / PCM8 の資料と
  VGM / MDX のファイル置き場）。
- `*.wav.zst` は除外していないので、`test/lfo_noise/wav/` のキャプチャ結果は
  git の管理対象に入る。
