# pico-opm-writer（ファームウェア内部設計）

[pico-opm-writer.c](../pico-opm-writer.c) ほかリポジトリ直下の `.c` / `.h` / `.pio`

Raspberry Pi Pico 2 (RP2350) 上のファームウェアが、OPM のバス制御と YM3012 の
DAC キャプチャをどう実装しているかの説明。**使い方・配線・コマンド仕様は
[README.md](../README.md)** にある。

## 1. ソース構成

| ファイル | 役割 |
| --- | --- |
| [pico-opm-writer.c](../pico-opm-writer.c) | `main()`、システムクロック設定、初期化、メインループ、行入力とコマンドパーサ、応答出力 |
| [opm.h](../opm.h) / [opm.c](../opm.c) | OPM のピン割り当て・タイミング定数・φM の分周比算出と張り替え、バス制御 |
| [opm_clock.pio](../opm_clock.pio) | φM 生成用 PIO プログラムと `opm_clock_program_init()` |
| [clockmode.h](../clockmode.h) / [clockmode.c](../clockmode.c) | φM プリセットの実行時切り替え。sys_clk と PIO 分周比の張り替え順序（[§2.3](#23-実行時の切り替え)） |
| [ym3012.h](../ym3012.h) / [ym3012.c](../ym3012.c) | YM3012 の PIO + DMA リング初期化、リング位置の管理、PCM 変換、自己診断 |
| [ym3012.pio](../ym3012.pio) | キャプチャ用 PIO プログラムと `ym3012_capture_program_init()` |
| [capture.h](../capture.h) / [capture.c](../capture.c) | キャプチャの状態機械と、リング → PCM → CDC #1 の送出 |
| [i2s.h](../i2s.h) / [i2s.c](../i2s.c) | I2S 出力の PIO + DMA リング初期化、先行量の維持、アンダーラン復帰 |
| [i2s.pio](../i2s.pio) | I2S 出力用 PIO プログラムと `i2s_out_program_init()` |
| [usb_pcm.h](../usb_pcm.h) / [usb_pcm.c](../usb_pcm.c) | CDC #1 の接続判定・書き込み・滞留量 |
| [led.h](../led.h) / [led.c](../led.c) | 非ブロッキングな LED パターン表示 |
| [stats.h](../stats.h) / [stats.c](../stats.c) | CPU 使用率・high-water・カウンタ |
| [flash_disk.h](../flash_disk.h) / [flash_disk.c](../flash_disk.c) | 内蔵フラッシュ上のブロックデバイス。領域定数、ライトバックキャッシュ、消去と書き込み |
| [ffconf.h](../ffconf.h) | FatFs の設定（上流の `external/fatfs/` には置かない。[§8.1](#81-ffconfh-をプロジェクト側に置く仕組み)） |
| [diskio_flash.c](../diskio_flash.c) | FatFs の `disk_*` 実装 |
| [storage.h](../storage.h) / [storage.c](../storage.c) | ストレージのモード状態機械、マウント、フォーマット、状態表示 |
| [filelist.h](../filelist.h) / [filelist.c](../filelist.c) | FatFs 上のファイル一覧の出力。`vgm list` / `mdx list` で共用（[§1.3](#13-ファイル一覧の共用)） |
| [usb_msc.c](../usb_msc.c) | USB マスストレージの `tud_msc_*` コールバック |
| [vgm.h](../vgm.h) / [vgm.c](../vgm.c) | VGM のヘッダ解析、コマンド解釈、スケジューラ、一覧 |
| [vgz.h](../vgz.h) / [vgz.c](../vgz.c) | gzip ストリームの展開。`FIL` から読み、展開したバイト列を前から順に返す（[§9.4](#94-vgz-のストリーム展開)） |
| [mdx.h](../mdx.h) / [mdx.c](../mdx.c) | MDX のヘッダ解析、MML の解釈、チャンネル状態機械、tick スケジューラ、一覧（[§10](#10-mdx-再生)） |
| [pcm8.h](../pcm8.h) / [pcm8.c](../pcm8.c) | MDX の ADPCM パート。PDX のストリーム読み出し、MSM6258 デコード、FM への加算（[§10.7](#107-adpcm-pcm8-の再生)） |
| [autoplay.h](../autoplay.h) / [autoplay.c](../autoplay.c) | VGM / MDX の自動連続再生。プレイリスト、曲順、曲送りの状態機械（[§11](#11-自動連続再生-autoplay)） |
| [tusb_config.h](../tusb_config.h) / [usb_descriptors.c](../usb_descriptors.c) | USB CDC 2 本 + MSC 1 本の TinyUSB 設定とディスクリプタ |
| [external/fatfs/](../external/fatfs/) | FatFs R0.16（上流のまま。[external/README.md](../external/README.md)） |
| [external/miniz/](../external/miniz/) | miniz 3.1.2（上流のまま。展開器 `tinfl` だけを使う。[external/README.md](../external/README.md)） |

### 1.1 メインループ

メインループは `tud_task()` → キャプチャリング位置の取り込み → ADPCM のレンダリング →
キャプチャの送出 → I2S への供給 → VGM の発行 → MDX の発行 → ストレージの書き出し →
LED → 統計 → コマンド 1 文字読み、を回すだけ。

**リング位置の取り込みと ADPCM のレンダリングが先頭に来る**のは、キャプチャも I2S も
`ym3012_reader_read_pcm()` の中でミックス済みの PCM を受け取るため。どちらが読むより
先に描き終えていなければならない（[§10.7](#107-adpcm-pcm8-の再生)）。
`d` の待機、`p 0` のドレイン待ち、`vgm list` / `mdx list` の行出力の合間からも
同じ処理を呼ぶので、
コマンドの待ち時間の中でも PCM の送出・I2S への供給・USB の処理は止まらない。

**この周回は 2 つの DMA リングの位置を追いかけている**（キャプチャ側とI2S 側）。
どちらもリング一周は 65.5ms なので、1 周回がこれを超えると位置を見失う。
フラッシュの消去はこれを超えるので、専用の復帰手順を用意している（[§9.3](#93-書き込み中の停止とリング位置)）。

**各サービスは毎周回ではなく、グループごとに最短間隔を決めて呼ぶ。** 間隔と決め手は
[README §3.11.1](../README.md#3111-サービスの呼び出し間隔) にある。判定は `due()` が
受け持ち、「最短でもこの間隔を空ける」であって「この周期ちょうどで回す」ではない。
長く止まったあとは 1 回だけ真になり、取りこぼしを溜めない。

間引く理由は 2 つ。1 つは、フレームが 62500/s しか届かないのにループは数十万周/秒
あるので、毎周回回すと 1 回あたり 1 フレーム未満のために毎回支度をすることになる点
（[§10.7](#107-adpcm-pcm8-の再生) の `PCM8_BATCH_FRAMES` と同じ理屈）。もう 1 つは、
**ここはビジーループなので 1 周が軽くなるとその分だけ周回数が増える**点。片方のグループ
だけを間引くと、浮いた時間を残りのグループが周回数ぶん食い返す（実測: 音声チェーンだけを
間引いたら `LOOP` が 3.4 倍になり `tud_task()` の占有率が 15% から 26% へ上がった）。
だから `tud_task()` も含めて全部を間引く。

コマンド側の入出力はすべて標準入出力 API 経由で行う。

- 出力: `printf()` / `puts()`
- 入力: `getchar_timeout_us()` でノンブロッキングに 1 文字ずつ受け取り、行バッファに溜める。
  受信 FIFO が空のときに SDK のドライバ走査まで降りないよう、`tud_cdc_n_available()` で
  先に見る
- 接続検出: `stdio_usb_connected()`（100ms ごと）

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

フラッシュ書き込みのように長く止まったあとは、リングの位置を張り直す
（[§9.3](#93-書き込み中の停止とリング位置)）。呼び出し側は集約した 1 本だけを使う。
個別の 3 本を直接呼ばないのは、消費者が増えたときに呼び忘れが起きないようにするため。

```c
void capture_resync_after_blackout(void); // 下の 3 つをまとめて呼ぶ。呼び出し側はこれだけ

void ym3012_ring_resync(void); // 書き込み位置を取り込み直し、既定カーソルを合わせる
void i2s_resync(void);         // 読み出し位置を取り込み直し、先行分を無音で埋め直す
void pcm8_resync(void);        // ミックスリングを無音で埋めてレンダリング位置を張り直す

void i2s_set_enabled(bool);    // 無効中はソースを読まず無音だけを流す（クロックは止めない）
```

変換した PCM に別の音源を混ぜるフック。USB キャプチャと I2S の唯一の合流点に
掛かるので、1 箇所で両方に効く（[§10.7](#107-adpcm-pcm8-の再生)）。

```c
void ym3012_set_mixer(ym3012_mixer_t fn); // 変換直後に呼ぶ。加算と飽和はフック側で行う
void ym3012_set_mix_ready(uint64_t frame);// 描き終えた位置。カーソルはここを超えて読まない
```

ミキサの後段に掛ける出力ゲイン。autoplay のフェードアウトがこれを使う
（[§11.3](#113-フェードアウト)）。

```c
void ym3012_fade_start(uint64_t start_frame, uint32_t frames);          // 1.0 -> 0.0
void ym3012_fade_release(uint32_t delay_frames, uint32_t ramp_frames);  // 猶予のあと 1.0 へ
void ym3012_fade_clear(void);                                           // 猶予もランプも無し
```

### 1.3 ファイル一覧の共用

`vgm list` と `mdx list` は、ディレクトリ・拡張子・件数上限を除いて同じ処理をするので
[filelist.c](../filelist.c) にまとめてある。`vgm_list()` / `mdx_list()` は引数を渡すだけ。

```c
const char *filelist_print(const char *dir, const char *const *exts, uint32_t n_exts,
                           uint32_t max_entries, void (*tick)(void));
```

名前を全部ためてからソートすると数十 KB のバッファが要るので、「直前に出した名前より
大きいものの中で最小」を毎回ディレクトリ走査で探す。必要な RAM は名前 2 個ぶんで、
これは `FILINFO.fname` と同じ `FF_LFN_BUF + 1`（256 バイト）。

**並べ替えの比較子と等値判定は別に持つ。**

- `filelist_name_cmp()` は大小無視で比較し、**大小無視で等しいときは `strcmp()` の結果を
  返す**。上の走査は順序が一意に決まらないと同じ名前を出し続けるか取りこぼすため。
- 拡張子の判定にはこれを使えない。`".MDX"` と `".mdx"` は大小無視では等しくても
  `strcmp()` は 0 を返さないので、一致しなくなる。等値判定は専用の `ext_equal()` が行う。

`tick` は 1 行出すごとに呼ばれる。呼び出し側は `service_all` を渡していて、
一覧の出力中も PCM の送出と I2S への供給が止まらないようにしてある。

#### 一覧を RAM へ集める

autoplay のプレイリストは任意の位置の名前を後から引く必要があり、上の「毎回全走査」では
足りない。同じフィルタを使いつつ、1 回の走査で呼び出し側のバッファへ詰める口を別に持つ。

```c
const char *filelist_collect(const char *dir, const char *const *exts, uint32_t n_exts,
                             uint32_t name_max, filelist_buf_t *buf);
```

- 名前は `'\0'` 区切りでプールへ、オフセットは `uint16_t` の配列へ入れる。走査しながら
  **二分挿入**するので、全部ためてから並べ替えるための追加バッファが要らず、動かすのは
  `uint16_t` の配列だけで済む。
- `buf->count` をリセットせず**追記**し、**今回足した範囲だけ**を並べる。`vgm_collect()` →
  `mdx_collect()` の順に呼べば「`/VGM` の昇順 → `/MDX` の昇順」が 1 個のバッファにできる
  （[§11.1](#111-プレイリスト)）。
- `name_max` より長い名前は飛ばす。`vgm_play()` / `mdx_play()` が 63 文字までしか
  受け付けないので、集めても鳴らせない。

`FILINFO` は 300 バイト近くあるのでスタックには置かず、`filelist_print()` と `static` で
共用する。**この 2 本は互いに再入できない。**


## 2. φM の生成

PIO ステートマシン 1 基で GPIO をトグルする（1 命令 1 サイクル × 2 で 1 周期）。

```
φM = sys_clk / (2 × clkdiv)
```

プリセットの一覧と切り替え方は [README §2](../README.md#2-クロック設定-φm) にある。
プリセットは動作させたまま切り替えられる（[§2.3](#23-実行時の切り替え)）。

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
呼んでいる（ここで決まるのは起動時のプリセットだけで、以後は
[clockmode.c](../clockmode.c) が張り替える）。USB は独立した USB PLL から給電されるため、この変更の影響を受けない。
既定の 144MHz は RP2350 の定格 150MHz 以内なのでオーバークロックにあたらない。
`OPM_CLOCK_MODE_NTSC` の 157.5MHz は定格に対して約 5% のオーバークロックになるが、
電圧設定の変更なしで動作する範囲。

`OPM_CLOCK_HZ` / `OPM_SYS_CLOCK_KHZ` をプリセット外の値へ直接書き換えることもできるが、
その場合は **必ず両方を整数分周になる組で**指定すること（片方だけの変更は小数分周になる）。

### 2.2 小数分周へのフォールバック

プリセット外の φM を指定して整数分周にならない場合は、PIO の小数分周
（`sm_config_set_clkdiv_int_frac8()`、分解能 1/256）にフォールバックする。起動時の
情報表示で実際の分周比を確認できる。

### 2.3 実行時の切り替え

4MHz と 3.579545MHz (= 315/88 MHz) の**両方**が整数分周になる sys_clk は 2520MHz が
最小で、RP2350 では実現できない。したがって φM を変えるには sys_clk ごと張り替えるしかない。
[clockmode.c](../clockmode.c) がこれを行う。

守るべきは 1 つ。**φM・BCK・LRCK の H/L 期間を公称値より短くしないこと。**
YM2151 と PCM5102A が短いパルスを取りこぼしたり誤判定したりするのを避ける。

#### 短いパルスが出る条件

φM の H/L 期間は `clkdiv` 個の clk_sys サイクルに等しい（[opm_clock.pio](../opm_clock.pio) は
2 命令ループ）。BCK も同じで、H/L = `clkdiv_i2s` 個。したがって:

- clk_sys 自体には欠けたサイクルが出ない。clk_sys は **glitchless mux** を持つ
- clk_sys が**下がる**方向では、進行中の H/L 期間は**伸びる**だけ
- clk_sys が**上がる**方向では、進行中の H/L 期間は最悪
  `clkdiv × 新 clk_sys の周期` まで縮む。**これは新しい周波数での公称 H/L 期間そのもの**

つまり短いパルスは「分周比が小さいまま clk_sys が上がる」ときにだけ出る。規則はこうなる。

> **clk_sys を上げる操作の直前には、その clk_sys に対応する最終の分周比が入っていること。
> 分周比を書き換えるのは clk_sys を下げたあとにすること。**

#### 手順

割り込みを落として一気に実行する。所要は数百 µs。

| | 操作 | そのあいだの φM |
| --- | --- | --- |
| A | clk_sys を pll_usb (48MHz) へ退避する。pll_sys を触るには使用をやめる必要がある | 下げ方向。H/L は伸びるだけ |
| B | 48MHz を前提に分周比を張り直す。**整数へ切り上げる**ので φM は目標以下にしかならない | ほぼ公称値（4MHz なら clkdiv 6 でぴったり、3.579545MHz なら clkdiv 7 で 3.429MHz） |
| C | `pll_init(pll_sys, ...)` で pll_sys を新しい周波数へ再ロックする | B のまま。OPM も DAC も通常の動作範囲に留まる |
| D | 新しい sys_clk を前提にした**最終の**分周比を入れる | 48MHz のままなので 1.1〜1.3MHz へ落ちるが、E までは約 1µs |
| E | clk_sys を pll_sys へ戻す | 上げ方向。最終の分周比が入っているので最悪でも公称値ちょうど |

`clock_configure_undivided()` は内部で一度 clk_ref (XOSC 12MHz) を経由する
（aux mux は glitchless ではないため、SDK が SRC を逃がしてから aux を書き換える）。
このため A と E それぞれに数 µs の「clk_sys 12MHz」の窓ができるが、これも下げ方向なので
短いパルスにはならない。E で戻るときは最終の分周比が効く。

`clk_peri` と `clk_ref` は起動時の設定（pll_usb / XOSC）のままで触らない。

#### 分周比の書き換え自体

走ったままの SM に `SMx_CLKDIV` を書くと、進行中のカウントがどう扱われるかは規定されて
いない。**SM を一瞬止めてから書く**ことでこの不確定性を消している。

```c
pio_sm_set_enabled(pio, sm, false);   /* ピンは最後のレベルを保持 = H/L が伸びる */
pio_sm_set_clkdiv_int_frac8(pio, sm, div_int, div_frac8);
pio_sm_clkdiv_restart(pio, sm);       /* 分周カウンタを 0 に。次の H/L は必ず全幅 */
pio_sm_set_enabled(pio, sm, true);
```

`pio_sm_clkdiv_restart()` は `CTRL.CLKDIV_RESTART` を立てるだけで PC / X / Y / ISR / OSR
には触らない。**`pio_sm_restart()` は使わない**（[i2s.pio](../i2s.pio) の注意書きのとおり、
PC を先頭へ戻すと X が中途半端なまま位相が恒久的に狂う）。

#### 影響を受けない部分

| | 理由 |
| --- | --- |
| ym3012 キャプチャ | [ym3012.pio](../ym3012.pio) は clkdiv 固定 1 で、SO / φ1 / SH1 / SH2 のエッジ駆動。12MHz の窓のあいだだけ φ1 に対する余裕が減って数フレーム壊れうるが、毎フレーム SH1 で再同期する |
| VGM のテンポ | `time_us_64()` 基準。タイマのティックは clk_ref (XOSC) 系なので sys_clk と無関係（[§9.2](#92-スケジューラ)） |
| USB | `clk_usb` は pll_usb 系で、pll_sys の再構成に影響されない |
| `OPM_T_DATA_US` = 25µs | 68 φM サイクルは 4MHz で 17µs / 3.579545MHz で 19µs。φM を下げる方向なので余裕は増える |
| I2S の先行量 | 切り替え中はメインループが約 300µs 止まるが、先行 `I2S_TARGET_FRAMES` = 16.4ms に対して十分短い。アンダーランしない |
| フラッシュ (QMI) | 分周は `PICO_FLASH_SPI_CLKDIV = 4` 固定で clk_sys に追従する。遷移の両端 36MHz / 39.4MHz はどちらも起動時に実績のある構成 |

`sys_clk` に依存して実行時に算出している値（[opm.c](../opm.c) の `s_setup_cycles` と
φM の分周比、[i2s.c](../i2s.c) の `s_div256`）は、切り替えのたびに計算し直す。
I2S 側は `clkdiv_i2s = 2 × clkdiv_opm`（[§5.2](#52-サンプリングレートのロック)）の関係だけで決まるので、
B で使う切り上げた分周比にもそのまま追従する。

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

MDX の ADPCM パートを鳴らしているあいだは、この変換の**直後**に ADPCM が加算される
（[§10.7](#107-adpcm-pcm8-の再生)）。加算はカーソルによらず同じ結果になるので、
USB キャプチャと I2S には同じ音が乗る。禁止コード E=0 の計数は加算より前に済ませて
あるので、統計は取り込んだ信号の品質を表したまま変わらない。

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

MDX の ADPCM パートは変換の直後に足されるので、I2S にも USB キャプチャと同じ音が
乗る（[§10.7](#107-adpcm-pcm8-の再生)）。

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

2 プリセットではどちらも整数分周になる。

| プリセット | sys_clk | φM | fs | clkdiv_i2s | BCK (32fs) |
| --- | --- | --- | --- | --- | --- |
| `4` | 144 MHz | 4.000000 MHz | 62500.0 Hz | 36 + 0/256 | 2.000 MHz |
| `3.58` | 157.5 MHz | 3.579545 MHz | 55930.4 Hz | 44 + 0/256 | 1.7898 MHz |

`i2s_retune()` は同じ式を再計算して、走っている SM の分周比だけを書き換える。
φM 側が何を入れていても `clkdiv_i2s = 2 × clkdiv_opm` の関係だけで決まるので、
クロック切り替えの途中で使う切り上げ分周比にも自動で追従する（[§2.3](#23-実行時の切り替え)）。

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

## 6. USB CDC 2 本と MSC の実装

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
食い潰す。同じ理由で `vgm list` は行の出力ごとにメインループのサービスを回す。

### 6.1 MSC の追加

マスストレージは CDC の**後ろ**（インタフェース 4、EP `0x05` / `0x85`）に足す。
CDC #0 をインタフェース 0 に固定したままにするためで、CDC #1 の番号も動かない。
`USBD_ITF_MAX` がそのまま `bNumInterfaces` になるので、enum に 1 行足すだけで追従する。

`CFG_TUD_MSC_EP_BUFSIZE` は論理セクタと同じ 512 にする。こうすると `read10` / `write10`
コールバックの `offset` が常に 0 になり、1 回の呼び出しがちょうど 1 セクタになる。
この define が無いと `msc_device.h` が `#error` で止まる。

**モードの切り替えでインタフェースを付け外ししない。** `bNumInterfaces` を変えるには
`tud_disconnect()` / `tud_connect()` が必要で、`storage host` と打った瞬間に CDC #0 が
切れてしまい、同じセッションからモードを往復できなくなる。代わりに
`tud_msc_test_unit_ready_cb()` の真偽で**メディアの挿抜**として表現する。
false を返すと TinyUSB が自分で MEDIUM NOT PRESENT の sense を立てるので、
PLAYER モードでは PC からの SCSI コマンドが `read10` / `write10` に到達する前に全部失敗する。

### 6.2 SCSI トレース

MSC はホスト側の挙動が見えないので、コールバックが受け取った SCSI をリングに控える
（`usb_msc.c` の `trace()`、直近 320 件）。表示は `storage trace`
（[README §7.4](../README.md#74-マウントされないときの調べ方)）。

**記録だけしてその場では表示しない。** MSC のコールバックは `tud_task()` の中から
呼ばれるので、ここで `printf` すると `stdio_usb` が `tud_task()` を再入しうる。

記録の開始は `storage_set_host()` が `usb_msc_trace_reset()` を呼ぶところ。
PLAYER に戻しても消さないので、イジェクトされたあとでも読み出せる。

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
  `hardware_clocks` / `hardware_flash` / `pico_flash` / `tinyusb_device` / `fatfs` / `miniz`
- FatFs と miniz は `add_library(... STATIC ...)` の別ターゲットにする。`-Wall -Wextra` が
  `pico-opm-writer` に `PRIVATE` で付いているので、これで上流コードに波及せず、
  警告抑止も改変も要らなくなる（[§8](#8-ストレージ)）
- miniz は設定ヘッダを持たないので、`miniz` ターゲットの `target_compile_definitions` で
  `MINIZ_NO_STDIO` / `MINIZ_NO_TIME` / `MINIZ_NO_MALLOC` / `MINIZ_NO_DEFLATE_APIS` /
  `MINIZ_NO_ARCHIVE_APIS` / `MINIZ_NO_ZLIB_APIS` を定義し、展開器 `tinfl` だけを残す
- キャッシュ変数 `OPM_CLOCK_MODE` を持ち、指定時のみ同名マクロを
  `target_compile_definitions` で渡す（φM プリセットの切り替え、[§2](#2-φm-の生成)）
- キャッシュ変数 `I2S_ENABLED`（既定 1）は**常に**マクロとして渡す。
  [ym3012.h](../ym3012.h) がこれを見て `YM3012_LOOPBACK_ENABLED` の既定値を決めるため、
  未定義のままにできない（[§4.6](#46-起動時の自己診断)）
- キャッシュ変数 `YM3012_LOOPBACK` は指定時のみ `YM3012_LOOPBACK_ENABLED` として渡し、
  上の自動判定を上書きする
- キャッシュ変数 `VGM_VGZ_ENABLED`（既定 1）は**常に**マクロとして渡す。
  [vgz.h](../vgz.h) がこれで実装ごと切り替わるため、未定義のままにできない
  （0 にすると展開器と約 86KiB のバッファがリンクされない。[§9.4](#94-vgz-のストリーム展開)）
- キャッシュ変数 `MDX_ENABLED`（既定 1）は**常に**マクロとして渡す。
  [mdx.h](../mdx.h) がこれで実装ごと切り替わるため、未定義のままにできない
  （0 にするとシーケンサと 64KiB のファイルバッファがリンクされず、`mdx` コマンドは
  すべて失敗する。[§10](#10-mdx-再生)）
- FatFs 領域は `FLASH_FATFS_RESERVE_KB`（ファームウェアに残す KiB。既定 256）から
  オフセットとサイズを CMake 側で計算し、`FLASH_FATFS_OFFSET` / `FLASH_FATFS_SIZE` /
  `FLASH_FATFS_TAIL_RESERVE` を**常に**マクロとして渡す。後段のリンク後チェックが
  領域の実値を必要とするため、[flash_disk.h](../flash_disk.h) 側の `#ifndef` 既定値は
  CMake を通さないビルドのための保険という位置づけ（[README §7.1](../README.md#71-領域の変え方)）。
  `FLASH_FATFS_OFFSET` / `FLASH_FATFS_SIZE` をバイトで名指しすることもでき、
  `FLASH_FATFS_OFFSET` と `FLASH_FATFS_RESERVE_KB` の同時指定は `FATAL_ERROR` にする
- `pico_add_extra_outputs()` の後に [cmake/check_flash_region.cmake](../cmake/check_flash_region.cmake)
  を `POST_BUILD` で走らせ、`__flash_binary_end` と領域が重なっていないかを検査する（§8.5）
- `PICO_STDIO_USB_STDOUT_TIMEOUT_US=10000` を定義する（[§6](#6-usb-cdc-2-本と-msc-の実装)）
- `pico_enable_stdio_usb 1` / `pico_enable_stdio_uart 0`
- `target_include_directories` にリポジトリ直下を入れる。SDK の tusb_config.h は
  `-isystem` で入るので、これで自前の [tusb_config.h](../tusb_config.h) が優先される

### 7.1 PIO のビルドフロー

`.pio` は `pico_generate_pio_header()` で `build/<名前>.pio.h` に変換され、C 側は
`#include "opm_clock.pio.h"` のように参照する。`.pio` 内の `% c-sdk { ... %}` ブロックは
そのまま生成ヘッダへ展開されるため、`opm_clock_program_init()` のような初期化ヘルパは
ここに書く。

**新しい `.pio` を追加したときは `pico_generate_pio_header()` の行を足して再コンフィグする。**

## 8. ストレージ

内蔵 QSPI フラッシュの一部を FAT ファイルシステムとして使う。層の構成は次のとおり。

```
  VGM Player                USB MSC (usb_msc.c)
      |                            |
    FatFs (external/fatfs)         |
      |                            |
  diskio_flash.c                   |
      |                            |
      +---------> flash_disk.c <---+
                       |
             内蔵 QSPI フラッシュ
```

`flash_disk.c` は論理セクタ 512 バイトのブロックデバイスで、FatFs と USB MSC の
**両方が同じサイズで見る**。所有権は `storage.c` のモードで排他にしており、
同時に両方から触られることはない（[README §7.3](../README.md#73-書き込みの仕組みと制約)）。

### 8.1 `ffconf.h` をプロジェクト側に置く仕組み

FatFs 本体は無改変で `external/fatfs/` に置く。設定だけをリポジトリ直下の
[ffconf.h](../ffconf.h) に持たせるために、**上流のテンプレート `ffconf.h` を
`external/fatfs/` へ置かない**。

`ff.h` の `#include "ffconf.h"` がツリー内で唯一の参照で（`ff.c` は `ff.h` と
`diskio.h` しか include しない）、GCC は `"..."` をまず include 元のファイルがある
ディレクトリから探す。そこに無ければ `-I` に入っているリポジトリ直下が拾われる。

事故を検出するため [ffconf.h](../ffconf.h) は `OPM_FFCONF_H` を定義し、
[diskio_flash.c](../diskio_flash.c) が `#if !defined(OPM_FFCONF_H)` で `#error` にする。
`ff.h` 自身も `FF_DEFINED != FFCONF_DEF` をコンパイル時に弾くので、版のずれも検出される。

出所・適用した公式パッチ・省いたファイルは [external/README.md](../external/README.md) に記録している。

### 8.2 ライトバックキャッシュ

フラッシュの消去単位は 4096 バイト、論理セクタは 512 バイトなので、書き込みには
読み出し・修正・書き戻しが要る。8 行 × 4KiB（RAM 32KiB）のキャッシュを挟む。

| 関数 | 性質 |
| --- | --- |
| `flash_disk_read()` | dirty な行はその行から、それ以外は XIP から `memcpy`。ブロックしない・消去しない・キャッシュを汚さない |
| `flash_disk_write()` | キャッシュに載せるだけ。**絶対に消去しない**。空きが無ければ `FLASH_DISK_BUSY` |
| `flash_disk_flush_one()` | 最も古い dirty 行を 1 つ書き出す。**唯一の停止点** |

行数を 8 にしてあるのは、FAT12 のメタデータ（ブートセクタ + FAT + ルートディレクトリ
= 4KiB ブロックで 5 個ほど）とデータクラスタを同時に載せておくため。PC のファイルコピーは
FAT・ディレクトリ・データを交互に書くので、行が少ないとそのたびに追い出しが起きる。

書き出す前に XIP を読んで全 `0xFF` なら消去を省く。新品の基板では領域が
まるごと消去済みなので、初回のフォーマットと最初のコピーが目に見えて速くなる。

`flash_range_program()` へ渡すポインタは RAM を指している必要がある（書き込み中は
XIP が止まる）。キャッシュ行のデータ部を別配列にしてあるのはそのため。

### 8.3 書き出しをいつ行うか

**`tud_msc_write10_cb()` の中でその場で書き出す。** 当初は「キャッシュが埋まったら 0 を
返して呼び直してもらい、その隙にメインループのサービスが書き出す」設計にしたが、これは
成立しない。`tud_task()` はイベントキューを空になるまで同じ呼び出しの中で回すので、
0 を返してもメインループには戻らず、再試行が空回りするだけでコピーが止まる。

HOST モードでは PCM キャプチャと I2S を止めてあるので（[§8.4](#84-所有権の切り替え)）、
ここで数十 ms ブロックしても巻き込む相手がいない。

先回りの書き出しはしない。「dirty が全行に達したら書き出す」ようにすると、ホストが
FAT やディレクトリを何度も書き直すたびに書き出したそばから汚れ直し、書き込み回数が
数倍になる。SCSI の `SYNCHRONIZE CACHE` でも書き出さない（macOS はコピー中にこれを
頻繁に投げてくる）。確実に書き切るのは eject と `storage player` のときだけ。

256KiB のコピー（macOS の `cp`）での実測:

| 方式 | 書き出し回数 | 所要時間 |
| --- | --- | --- |
| 先回り + SYNCHRONIZE CACHE で全書き出し | 640 | 34 秒 |
| 必要になったときだけ | **76** | **3 秒** |

理論値は 256KiB / 4KiB = 64 回なので、1.19 倍まで詰められている。

### 8.4 所有権の切り替え

```
PLAYER --( storage host )--> HOST
    ガード: VGM 停止中 かつ capture_state() == IDLE
    動作:   i2s_set_enabled(false) -> f_unmount() -> メディア挿入

HOST --( storage player / ホストの eject )--> PLAYER
    動作:   flash_disk_flush_all() -> メディア排出 -> f_mount()
            -> i2s_set_enabled(true) -> capture_resync_after_blackout()
```

`STORAGE_MODE_PLAYER` を 0 にしてあるので、`.bss` のゼロ初期値が「メディア非挿入」に
なる。`tusb_init()` から `storage_init()` までの隙間で MSC のコールバックが呼ばれても
安全側に倒れる。

`disk_status()` は HOST モード中に `STA_NOINIT` を返し、FatFs 側からも同時アクセスを塞ぐ。

HOST へ入るとき `storage_take_media_change()` の合図を立て、MSC 側は最初の
TEST UNIT READY で 1 回だけ UNIT ATTENTION（`0x06` / ASC `0x28`「メディアが
入れ替わった」）を返す。`tud_msc_test_unit_ready_cb()` が false を返しても、
sense を先に立てておけば TinyUSB は MEDIUM NOT PRESENT で上書きしない
（`msc_device.c` は `sense_key` が 0 のときだけ既定値を入れる）。

またホストがメディアを 1 度も読んでいないうちに来た LOEJ は無視する。
これは利用者の操作ではなく「前に取り外したのだから入れ直すな」というホストの指示で、
従うと `storage host` に戻せなくなる。

**ただし macOS では、一度取り出すと USB を挿し直すまでディスクとして戻ってこない。**
UNIT ATTENTION を返しても、TinyUSB がリムーバブルメディアとして申告していても、
LOEJ 処理を完全に無効にしても変わらないことを実機で確認した。取り出した時点で
ホスト側の USB マスストレージドライバがデバイスから切り離されるためで、
ファームウェア側から回避する手段は無い（[README §7.2](../README.md#72-pc-から曲データをコピーする)）。

### 8.5 領域がファームウェアと重ならないこと

`storage_init()` でリンカシンボル `__flash_binary_end` を読み、領域の先頭を超えていたら
`STORAGE_FS_REGION_OVERLAP` にして**マウントも書き込みも一切しない**。

`hard_assert` で止めないのは、基板が起動しなくなると復旧しづらいため。状態として
持ち回り、`i` と `storage status` から常時見えるようにしている。

ただし実行時の検査は焼いて起動するまで結果が分からないので、**その前に 2 段構えで
落とす**。

領域自体の妥当性は [flash_disk.c](../flash_disk.c) の `_Static_assert` でコンパイル時に
検査する。4096 バイト境界に揃っているか、末尾の予約セクタに食い込んでいないか、
クラスタ数が FAT12 の上限（4085）を下回るか。

ファームウェアと重なっていないかは、リンクが終わらないと分からない。
[cmake/check_flash_region.cmake](../cmake/check_flash_region.cmake) を `POST_BUILD` で
走らせ、`${CMAKE_NM}` で `__flash_binary_end` を読んで領域の先頭と突き合わせる。
重なっていれば `FATAL_ERROR` でビルドが失敗し、そうでなければ余裕のバイト数を出す。

末尾の予約セクタ（`FLASH_FATFS_TAIL_RESERVE`、既定 4096）は picotool が UF2 へ入れる
RP2350-E10 の absolute block 用。0x10FFFF00 狙いのブロックが 4MiB のフラッシュでは
末尾セクタへ折り返すので、ここを領域に含めると UF2 で焼くたびにファイルシステムの
末尾が潰れる（[README §7.1](../README.md#71-領域の変え方)）。

## 9. VGM 再生

### 9.1 コマンドの解釈

`vgm_step()` が 1 コマンドずつ処理する。オペランド長は `operand_len()` の表で引き、
`0xFF` は「未知なので中断」。**どの経路も必ず 1 バイト以上消費する**ので、
壊れたファイルでも無限ループにならない。シークは必ずファイル全体の長さと突き合わせる
（`.vgz` では `f_size()` の代わりに gzip トレーラの ISIZE を使う。[§9.4](#94-vgz-のストリーム展開)）。

`0x54 aa dd`（YM2151 書き込み）の `aa` は 8bit のレジスタアドレスそのもので、
**bit7 をマスクしてはいけない**。`0x80`-`0xFF` は D1L/RR・KC・KF・PMS/AMS の実レジスタで、
落とすと音が出なくなる。2 個目の YM2151 は別オペコード `0xA4` で、固定長スキップで飛ばす。

`0x67` のデータブロックは MB 単位になりうるので、`f_lseek()` で飛ばす（1 バイトずつ
読んで飛ばすと破綻する）。`.vgz` はシークできないので別扱いになる（[§9.4](#94-vgz-のストリーム展開)）。

`vgm_play()` はヘッダ解析のあと `apply_file_clock()` を呼び、オフセット `0x30` が申告する
YM2151 のクロックへ φM を寄せる（[clockmode.c](../clockmode.c) の
`clockmode_follow_file()`。最寄りのプリセット、境界は 2 つの中点 3789772Hz）。
これは `opm_clear()` と `s_start_us` の設定より前に済ませるので、切り替えに使った
数百 µs が再生の起点に乗らない。寄せてもなお一致しないときだけ、残ったずれを警告する。

`clock fixed` のときは寄せずに警告だけを出す。クロックを切り替えられない状態
（PCM キャプチャ中）で切り替えが必要になった場合は、`vgm_play()` 自体が
`wrong state` で失敗する（[§2.3](#23-実行時の切り替え)）。

### 9.2 スケジューラ

```c
s_due_us = s_start_us + (s_samples * 1000000ull) / VGM_SAMPLE_RATE;
```

**絶対サンプル数から毎回計算し直す**ので丸め誤差が累積しない。ループの継ぎ目でも
`s_samples` と `s_start_us` を触らないため、時間の不連続も生じない。

1 回の `vgm_service()` は `VGM_BUDGET_US`（500µs）で打ち切る。メインループは 1 周回で
標準入力を 1 文字しか読まないので、ここを大きくするとコマンド入力が遅くなる
（1.5ms にすると 660 文字/秒まで落ちる）。500µs なら約 2000 文字/秒で、
追いつき能力は `opm_write()` が 32µs なので約 28k writes/s あり十分。

ハードウェアタイマの割り込みは使わない。`opm_write()` が 32µs ブロックするため、
キーオンが集中すると割り込み文脈を数 ms 占有して `i2s_service()` を飢えさせる。
「アプリコードに割り込みハンドラを置かない」という既存の方針も崩れる。

キャプチャのフレームカウンタ（φM/64）から時計を取る案も採らない。62500 → 44100 の
換算が結局必要なうえ、再生タイミングがキャプチャ経路の健全性に依存してしまう。

`VGM_RESYNC_LAG_US`（200ms）を超えて遅れたら、遅れを早送りで取り返さずに時計の方を
現在時刻へ張り直す。回数は `s` の `reslip` に出る。

### 9.3 書き込み中の停止とリング位置

**これを外すと既存機能が無警告で壊れるので、実装で最も注意を要する点。**

`ym3012_ring_poll()` と `i2s_poll()` は DMA ポインタの差分を**リング長 4096 フレームで
剰余を取って**総フレーム数に積む（[§4.4](#44-dma-リング) / [§5.3](#53-出力リングと先行量の維持)）。
これにより

```
s_write_total ≡ DMA の書き込み添字 (mod YM3012_RING_FRAMES)
s_dma_total / s_fill_total ≡ リング内の添字 (mod I2S_RING_FRAMES)
```

という**位相**が常に成り立つ。「フレーム番号 f の中身は `s_ring[f & 4095]` にある」という、
読み出しカーソル・フェードのゲイン・PCM8 のミックスリングが共通で頼っている前提は、
この位相そのものである。

**張り直すときも差分は必ず積む。** 基準点だけを現在位置へ寄せると、止まっていた分だけ
位相がずれる。位相がずれても音は連続したままなので気づかないが、以後フレーム番号が指すのは
その分だけ**古い音**になり、ずれは停止のたびに積み上がる。実害は次のとおり。

- **フェードの解除境界が過去に落ちる。** `ym3012_fade_release()` は「いま取り込んでいる
  フレーム」に境界を置くが、位相が E ずれていると実際に解除されるのは E フレーム前から。
  autoplay のフェードアウト直後に、キーオフ前の全音量の音楽が最大 65.5ms 出る
- **I2S の先行量が嘘になる。** `i2s_resync()` の直後に `s_fill_total = s_dma_total` する
  ので、真の先行量が `(I2S_TARGET_FRAMES - 止まっていた分) mod 4096` になる。`depth` は
  満量を報告し続けるので `depth <= 0` のアンダーラン復帰も発火しない
- **overrun 検出が手遅れになる。** 真の上書きは `unread >= 4096 - E` で始まるのに、
  判定は `unread >= 4096` のまま

一周 65.5ms を超えて止まると、進んだ分が一周単位で切り捨てられて総フレーム数
（`s` の `FRAMES` / `RATE`）が少なく出る。**それは統計だけの誤差で、位相はどれだけ
止まっても必ず合う。** フラッシュの書き出しは 1 セクタごとに張り直すので、実測でも
1 回あたり 42ms 止まりで一周には届かない。

対策は 2 段構えにしてある。

1. **HOST モード中は音声経路を止める。** `i2s_set_enabled(false)` で I2S はソースを
   読まず無音だけを詰める。リング全体が無音なので、停止中に DMA が古い内容を再生しても
   出てくるのは無音。BCK / LRCK と DMA は止めない（止めると PCM5102A がポップし、
   [i2s.h](../i2s.h) の「I2S 出力は常時動作し停止しない」前提も崩れる）
2. **`capture_resync_after_blackout()` で基準点を張り直す。**
   `flash_disk_flush_one()` の中（`storage format` は PLAYER モードで走るため必要）と、
   `storage player` への遷移時の 2 か所で呼ぶ

**張り直しは 3 本を 1 本にまとめてある**（[§1.2](#12-主要-api)）。`ym3012_ring_resync()` /
`i2s_resync()` / `pcm8_resync()` を個別に呼ばないのは、消費者が増えたときに呼び忘れが
起きないようにするため。`i2s_resync()` が**自分の ym3012 カーソルも同期する**のが肝で、
既存のアンダーラン復帰路ではやっているが停止経路はそこを通らない。前 2 者は
`ym3012_ring_poll()` / `i2s_poll()` を呼んでから復帰処理に入る形にしてあり、
位相を保つ責任は poll の 1 か所に閉じている。

検証は `s` を見る。**フラッシュの書き出し回数 (`FLASH WRITE`) と `I2S UNDERRUN` が
ほぼ同数**になり、復帰後に `RATE` が φM/64 ちょうどへ戻れば正しく繋がっている。
`UNDERRUN` が 0 のままなら resync が効いておらず、静かに壊れている。

**位相までは `s` からは見えない。** 崩れていても `RING` は空いて見え、`OVERRUN` も
増えないので、確かめるには音で測る。長い無音を作ってから `p 1` を打ち、その直後に
キーオンして、キャプチャの先頭からトーンが始まるまでのフレーム数を数える。
`capture_start()` は `ym3012_ring_poll()` と `ym3012_ring_sync()` を通るので、位相が
合っていればコマンドの往復ぶん（1ms 未満）に収まる。ここが数千フレームなら崩れている。

### 9.4 `.vgz` のストリーム展開

gzip 圧縮された VGM を、一時ファイルを作らずに展開しながら再生する。層の構成は次のとおり。

```
  vgm.c        ヘッダ解析・コマンド解釈・スケジューラ（圧縮の有無を知らない）
    |          s_buf[4096] の補充だけが分岐する
  vgz.c        gzip ヘッダ / トレーラの解析、出力リング、スナップショット
    |          VGM の知識は持たない
  miniz tinfl  DEFLATE の展開（external/miniz）
    |
  FatFs (FIL)  圧縮されたままのバイト列を読む
```

`vgm_getc()` 以降のコマンド解釈は非圧縮と共通で、`s_buf` も同じものを使う。
違うのは補充が `f_read()` か `vgz_read()` かだけ。

**圧縮の判定は拡張子ではなくファイル先頭のマジック（`1F 8B`）で行う。** 中身が gzip の
`.vgm` も、`.vgm` そのままの `.vgz` も世の中にあるため。`vgm list` と `has_vgm_ext()` は
拡張子で拾うが、それは一覧に何を出すかの話であって展開するかどうかとは独立している。

**出力リングが DEFLATE の履歴窓を兼ねる。** `tinfl` は出力バッファをリングとして扱い、
窓のマスクを

```c
mask = (pOut_buf_next - pOut_buf_start) + *pOut_buf_size - 1;
```

から作って、これが 2 の冪 -1 でないと弾く。**つまり「毎回リングの末尾まで埋めさせる」
呼び方しかできず、1 回の出力バイト数を選べない。** DEFLATE の距離は最大 32768 なので、
リングを 32KiB (`VGZ_DICT_SIZE`) にすればそれがそのまま履歴窓になり、別に窓を持つ必要も
無くなる。

出力長を選べない代わりに、**1 回の `tinfl_decompress()` の仕事量は入力量で抑える**。
`VGZ_IN_CHUNK`（256 バイト）だけ渡すと、通常の圧縮率（3〜10 倍）で 1〜3KiB の展開に
相当する。圧縮率が極端に高い箇所では 1 回でリング末尾まで（最大 32KiB）展開しうるが、
それでも数 ms で、I2S のリングが 65.5ms あるので破綻しない（[§5.3](#53-出力リングと先行量の維持)）。
圧縮率 72 倍のファイルを実機で再生したときの `VGM LAG` の最大値は 11ms、
`I2S UNDERRUN` は 0 だった。

`vgm.c` 側の補充単位も非圧縮とは別にしてある。`VGM_GZ_CHUNK` は 1024 バイトで、
展開が 1 バイトあたり十数サイクルかかるため `s_buf` 全体（4096 バイト）を一度に埋めると
`VGM_BUDGET_US`（500µs）を割ってしまう（[§9.2](#92-スケジューラ)）。

**ループの戻り方が、実装で最も注意を要する点。** gzip は後方シークできないので、
素直に作ると 2 周目の頭出しのたびに先頭から展開し直すことになり、継ぎ目で音が
数百 ms 途切れる。

そうならないよう、**ループ先頭 (`s_loop_target`) をはじめて通過する瞬間に展開器の状態を
丸ごと保存する**。保存するのは次の 5 つ。

| 保存するもの | 大きさ |
| --- | --- |
| `tinfl_decompressor`（ビットバッファ・ハフマン表・状態） | 約 8KiB |
| 32KiB の出力リングとその書き込み位置 | 32KiB |
| 消費済みの圧縮オフセット（ファイル内位置） | - |
| 展開後の位置と、未消費の展開データの位置・長さ | - |
| `vgm.c` 側の `s_buf` とその位置（`vgz` 側は `s_buf` に先読みしたぶん先を指しているため） | 4KiB |

2 周目以降は `memcpy` と `f_lseek()` で戻すだけなので、**継ぎ目に停止は出ない**
（非圧縮の `.vgm` と同じ）。これが成り立つのは `tinfl_decompressor` が**ポインタを含まない
POD** で、出力リングを呼び出しごとに引数で渡す造りだからで、miniz を更新するときは
この前提を確認する（[external/README.md](../external/README.md)）。

保存できないままループ先頭を過ぎてしまった場合の保険として `vgz_rewind()` があり、
先頭から展開し直す。この経路を通った回数は `s` の `gz reload` に出す
（[README §3.11](../README.md#311-s統計)）。**0 でなければループのたびに音が途切れている。**

**前方への読み飛ばしは展開して捨てるしかない。** `0x67` のデータブロックを gzip の
途中から飛ばすことはできないため。データブロックは MB 単位になりうるので、
1 回のサービスで消化しきると I2S が飢える。

`vgm_skip()` は残りバイト数を `s_skip_left` に積むだけにして、`vgm_step()` の頭で
`VGM_GZ_CHUNK`（1024 バイト）ずつ消化する。`vgm_service()` の 500µs 予算ループに
そのまま乗るので、読み飛ばしのあいだもキャプチャと I2S へのサービスが挟まる。
ここで生じた遅れは `VGM_RESYNC_LAG_US`（200ms）による時計の張り直しが吸収する
（[§9.2](#92-スケジューラ)）。256KiB のデータブロックを含む `.vgz` を実機で再生したときの
`VGM LAG` の最大値は 20ms、`I2S UNDERRUN` は 0 だった。

**gzip トレーラの CRC32 は検証しない。** 読むのは ISIZE（展開後サイズ）だけで、
`vgm_size()` が `f_size()` の代わりに使う。**ループ再生では終端に到達しないことが普通**で、
全部展開しないと計算できない CRC を待っても壊れたファイルの検出は早まらないため。
壊れていれば `tinfl` の展開エラーか、VGM 側の未知オペコード
（[§9.1](#91-コマンドの解釈)）として必ず捕まる。

`VGM_VGZ_ENABLED=0` にすると [vgz.c](../vgz.c) は「常に失敗する」スタブだけになり、
展開器も約 86KiB のバッファもリンクされない（[§7](#7-ビルド構成)）。
`vgm_play()` は gzip のファイルを `bad file` で拒否し、理由を `# hint` 行で出す。

## 10. MDX 再生

VGM が「レジスタ書き込みのタイムスタンプ付きダンプ」なのに対し、MDX は
**チャンネルごとに独立したシーケンサ**を持つバイナリ MML なので、音色・音量・ピッチ・
LFO をこちらで解釈してはじめて YM2151 のレジスタ値になる。つまり [mdx.c](../mdx.c) は
MXDRV 相当の音源ドライバそのもの。

利用者から見た仕様は [README §9](../README.md#9-mdx-再生)。

### 10.1 参照実装への準拠

レジスタへの書き込み内容と順序は **MXDRV 2.06+17 Rel.X5-S / MXDRVg V2.00b** に
合わせてある（一部の機能だけ **MXDRV 2.06+16 Rel.3+25**。[README §12](../README.md#12-ライセンス)）。
[mdx.c](../mdx.c) の冒頭に、参照実装のチャンネルワーク `MXWORK_CH` のオフセット
（`S0000`〜`S004e`）と `mdx_ch_t` のフィールドの対応表を置いてある。整数の幅も
参照実装に合わせ（符号なしで持ち、必要なところだけ符号付きへキャストする）、
68000 での桁溢れの仕方まで同じ結果になるようにしている。

**MXDRV のソースはリポジトリに含めない。** `mdx.c` は独自に書き起こしたもの
（[README §12](../README.md#12-ライセンス)）。

相違点は 1 つだけで、**BUSY 待ちをしない**こと。MXDRV は書き込みのたびに OPM の
ステータスを読んで bit7 が下りるのを待つが、本機はデータバスが出力専用でステータスを
読めないので、`opm_write()` の固定ウェイト（合計約 32µs、[§3.1](#31-タイミング定数)）で
代える。X68000 実機より遅い方向にしかならない。

参照実装が持つ 256 バイトのレジスタシャドウはそのまま持っている。レジスタ `0x1B` は
上位 2bit が CT1/CT2 出力で、LFO 波形を書くときに読み出して混ぜる必要があるため。

### 10.2 データ構造とローダ

ファイルは **丸ごと RAM に載せる**（`MDX_MAX_BYTES` = 64KiB）。VGM の 4096 バイト
前方ストリームは使えない。チャンネルごとに独立したポインタがリピートやループで前後し、
さらに `0xF6`（リピート開始）が**データ自身を書き換えて**回数カウンタに使うため。

ヘッダの構造は次のとおりで、オフセットとワードはすべてビッグエンディアン（68000）。

```
タイトル (Shift_JIS)  0x0D 0x0A 0x1A で終端
PDX ファイル名        0x00 で終端
base: +0      Word      音色データのオフセット（base 相対）
      +2      Word × N  各チャンネルの MML のオフセット（base 相対）
```

チャンネル数 `N` は表そのものの大きさから求める。表は base から `2 + 2N` バイトなので
`N = (先頭チャンネルのオフセット - 2) / 2` で、9（FM 8 + ADPCM 1）か
16（FM 8 + ADPCM 8。PCM8 拡張）になる。それ以外なら `bad file`。

音色は 27 バイト固定長レコードの並びで、先頭バイトが音色番号。番号 → 位置の索引を
256 エントリ作る。参照実装は再生のたびに線形探索して**最初の一致**を採るので、
索引も最初の一致を残す。

### 10.3 tick スケジューラ

テンポは OPM の Timer-B 由来で、1 clock は `1024 × (256 - @t)` φM サイクル。
**φM サイクル数でちょうど整数になる**ので誤差なく時刻へ直せる。

```c
s_due_q += ((uint64_t)period_cycles * 65536ull * 1000000ull) / phim_hz;
s_due_us = s_due_q >> 16;
```

`s_due_q` は 1/65536µs 単位の累算器。`period_cycles` は最大 262144 なので、除算前でも
u64 に収まる。丸めは 1 tick あたり 15ps 未満で、1 時間で 4µs 程度。テンポ変更にも
`clock` による φM の切り替えにも自然に追従する。

**1 tick は途中で中断して次の周回へ持ち越せる。** 音色の展開は 1 チャンネルあたり
25 レジスタ（約 800µs）で、複数チャンネルで同じ tick に重なると 1 tick で 180 回を
超える。`MDX_BUDGET_US`（500µs）で抜けて次の周回から続きを処理し、**全チャンネルの
処理が終わるまで tick は進めない**ので、演奏が先へ流れることはない。中断位置は
`s_cursor` と `s_in_tick` が持つ。

この予算の理由は `VGM_BUDGET_US` と同じで、メインループが 1 周回で標準入力を
1 文字しか読まないため（[vgm.h](../vgm.h) の説明）。

`MDX_RESYNC_LAG_US`（200ms）を超えて遅れたら、早送りせず時計を張り直す。

### 10.4 チャンネル状態機械

1 tick で各チャンネルに対して次の 3 つをこの順に呼ぶ（参照実装の割り込みハンドラと
同じ並び）。

1. **`ch_pre()`** — ポルタメントの積分と LFO を 1 clock 進める
2. **`ch_len()`** — ゲートタイムを減らして 0 ならキーオフ、音長を減らして 0 なら
   次の音符・休符まで MML を読む（`ch_fetch()`）。同期待ち中はここで止まる
3. **`ch_post()`** — 音色の展開・パン・音程・音量をレジスタへ反映し、キーオンする

**音色の書き込みはキーオンまで遅延する。** `0xFD`（`@`）は音色レコードの位置を控えて
フラグを立てるだけで、レジスタは書かない。参照実装と同じ挙動で、書き込みバーストの
分布が変わるとタイミングの詰まり方も変わるため合わせてある。

`0xF1 0x00`（演奏終了）に当たったチャンネルは、参照実装が静的な `{0x7f, 0xf1, 0x00}` へ
ポインタを向け直すのと同じ効果を、**いま読んだ `0xF1` の位置へ戻す**ことで作る。
こうしないとその先にある別チャンネルの MML や音色データを読んでしまう。
全チャンネルが終了したら再生を止める。

### 10.5 音程と音量の変換

内部の音程は **1/64 半音**単位。MDX の音符 0-95 は `音符 × 64 + 5` が基準で、`+5` は
MXDRV の調律ぶん。ここへディチューン・ポルタメント・音程 LFO を足し、
`pitch_to_kc_kf()` が 0x17FF で頭打ちにしてから 4 倍し、上位バイトを KC（`0x28+ch`）、
下位バイトを KF（`0x30+ch`）にする。KC の下位 4bit は 3 / 7 / 11 / 15 を飛ばすので
96 エントリの表で引く。MDX の音符 0 は `o0 d+` で OPM の KC 0 とちょうど一致する。

音量は `v0`-`v15`（16 段の表で TL へ）と `@v`（TL 直接）の 2 系統で、値の bit7 で
区別する。TL を足すのは**キャリアのスロットだけ**で、どれがキャリアかは音色の CON から
引く（`{0x08,0x08,0x08,0x08,0x0c,0x0e,0x0e,0x0f}`）。オペレータのレジスタは
`ベース + スロット × 8 + チャネル` で並びは M1 / M2 / C1 / C2、これは MDX の音色 27 バイトの
並びと同じなので並べ替えは要らない（[test/noise_period/](../test/noise_period/README.md)
で実測済み）。

この 2 つの変換は `t` コマンドが既知のベクタで検証する
（[README §3.12](../README.md#312-t自己テスト)）。

### 10.6 出力バックエンド

音を出す操作は `snd_voice()` / `snd_pan()` / `snd_volume()` / `snd_pitch()` /
`snd_key_on()` / `snd_key_off()` の 6 本に集約してある。**FM と ADPCM の分岐は
この 6 本の中だけ**にあり、`if (ch < 8)` を処理のあちこちに撒かなくて済む。

FM チャンネルは 6 本すべてが OPM のレジスタを書く。ADPCM チャンネルは
`snd_key_on()` / `snd_key_off()` だけが働き、[pcm8.c](../pcm8.c) を呼ぶ。
音色・パン・TL・KC/KF はレジスタに対応するものが無いので何もしない
（PCM8 は発音のたびに音量・レート・定位を引数で渡す方式で、レジスタ相当の
遅延書き込みが要らない）。

**ADPCM チャンネルもシーケンサとしては FM と同じように回す。** 同期コマンド
（`0xEF` / `0xEE`）が ADPCM 側から FM 側へ飛んでくる曲があり、止めると FM が
永久に待つため。チャンネルの内訳は参照実装と同じく 16 = FM 8 + ADPCM 1 + PCM8 拡張 7 で、
9-15ch は `0xE8`（PCM8 モード宣言）が実行されてから回り始める。

`0xE8` が変えるのはチャンネル数だけではない。**ADPCM の制御そのものが切り替わる**
（[§10.7](#107-adpcm-pcm8-の再生) の「PCM8 モードと IOCS 経路」）。

### 10.7 ADPCM (PCM8) の再生

X68000 の ADPCM は MSM6258 という別のチップで、本機には載っていない。
[pcm8.c](../pcm8.c) が **PCM8（江藤啓氏の ADPCM 多重再生ドライバ）相当の 8ch** を
ソフトウェアでデコードし、YM3012 から取り込んだ FM の PCM に足す。
利用者から見た仕様は [README §9.7](../README.md#97-adpcm-pcm8-の再生)。

```
  PDX（FatFs / 内蔵フラッシュ）
        │  チャンネルごとに 1KB ずつ先読み
        ▼
   ADPCM デコード（MSM6258）→ 音量 → hold 回そのまま繰り返す
        │
        ▼
   8ch を int32 で加算 → 1 回だけ飽和 → 16KB ミックスリング（フレーム番号で引く）
        │
        ▼
   ym3012_reader_read_pcm() の中で FM に加算 → 1 回だけ飽和
        │                                      │
        ▼                                      ▼
   USB キャプチャ (CDC #1)                  I2S 出力
```

#### PCM8 の呼び出し方

参照実装は `TRAP #2` で PCM8 を叩く。[pcm8.h](../pcm8.h) の API はその機能コードと
1 対 1 に対応させてある（`pcm8_key_on()` = `$000x`、`pcm8_set_mode()` = `$007x`、
`pcm8_stop()` = `$000x` でデータ長 0、`pcm8_abort_all()` = `$0101`）。

- **キーオンは 2 回発行する。** 参照実装はまずデータ長 0 で当該チャネルを止め、
  次に長さを付けて鳴らす。`snd_key_on()` もそのまま `pcm8_stop()` → `pcm8_key_on()` と呼ぶ。
- **音量**は FM と同じ `v` / `@v` の値から TL 相当（0-42）を作り、43 段の表で
  PCM8 の音量 0-15 に落とす。フェードアウトのオフセットもここに 8bit で足すので、
  溢れたら音量 0 かつ定位 0（= そのチャネルは鳴らない）になる。
  参照実装と同じく **フェードアウトはキーオン時にしか効かない**。IOCS 経路では
  音量そのものが指定できないので、代わりに `PCM_CUT` が定位を 0 にする。
- **定位は全チャネル共通**（PCM8 の仕様）。0 以外で最後に指定された値が残る。
  MML の `p` は `0xFC` の側で 0 と 3 を入れ替えて格納し、キーオン時にもう一度
  入れ替えるので元に戻る。初期値 0 はキーオン時に 3（左右）になる。
- **`v` / `@v` はタイでつながっている間だけ**、発音中の音量を `$007x` で差し替える
  （参照実装の `PCM_Vol`）。判定に使う「直前の音がタイだったか」は `ch_len()` の
  入口でチャンネルのレガート指定を写して持つ。周波数と定位は `-1`（据え置き）で
  渡すので、音量が範囲を外れても停止はせず音量 0 になるだけ。PCM8 モードでない曲では
  発行しない。
- **`0xE7 0x02`（PCM8 へのコマンド）** は 6 バイトが `d0.w` + `d1.l` の
  ビッグエンディアン。`$0100`（終了）・`$0101`（一時停止 = 即時打ち切り）・
  `$007x`（動作モード変更）を解釈し、本機に効く設定が無いもの（`$01FC` など）は無視する。

#### PCM8 モードと IOCS 経路

参照実装が PCM8 を叩くのは **`0xE8`（PCM8 モード宣言）が実行された曲だけ**。
`0xE8` が無い曲の ADPCM は IOCS `_ADPCMOUT` / `_ADPCMMOD` へ流れる。IOCS には
音量もバンクも無いので、**同じ PDX を使っても鳴り方が違う**。

| | PCM8 モード（`0xE8` あり） | IOCS 経路（`0xE8` なし） |
| --- | --- | --- |
| 発音チャンネル | 8（9-15ch） | 1（8ch のみ） |
| 音量 `v` / `@v` | 効く（0-15 に落とす） | **効かない**（原音量 8 固定） |
| フェードアウト | 音量に足す | **音量には効かない**。`PCM_CUT`（`FADE_ADD` >= 10）で定位 0 = 無音 |
| バンク `@n` | 効く（`bank × 96 + 音符`） | **効かない**（常にバンク 0） |
| PDX の長さ | 32bit を読んで 24bit へ切る | **下位 16bit だけ** |
| タイ中の音量差し替え | `$007x` を出す | 出さない |
| `0xE7 0x02`（PCM8 コマンド） | 実行する | 6 バイト読み飛ばす |
| キーオフ | データ長 0 のチャネル停止 | `_ADPCMMOD` の中止 + 終了 |
| `0xE7 0x06` | 効かない | 中止を抑止して鳴らし切る |

周波数（`0xED`）と定位（`p`）はどちらの経路でも同じ値を渡す。

[pcm8.h](../pcm8.h) では IOCS 経路を `pcm8_iocs_out()` / `pcm8_iocs_mod()` として
別の入口に分けてある（参照実装でも呼ぶベクタが違う）。MSM6258 は実機に 1 個しか
無いので `pcm8_iocs_out()` は ch0 固定。

#### PDX の読み出し

PDX は先頭が **96 音 × 8 バイト（BE の offset と length）のエントリ表**で、
`@n`（`0xFD`）が選ぶバンクを合わせると `(bank × 96 + 音符) × 8` で引ける。長さは
参照実装と同じく 24bit で切る。offset はファイル先頭からの絶対位置。
IOCS 経路ではバンクが効かず（常に 0）、長さも下位 16bit しか見ない。

**PDX は丸ごと RAM に載せない。** 数百 KB あって MDX 本体（64KiB）のようには
扱えないので、`FIL` を 1 本開いたまま `f_lseek` + `f_read` でストリーミングする。
チャンネルごとに 1KB の先読みバッファを持ち、使い切ったら次を読む
（15.6kHz でも 1KB は 131ms 分なので、補充は毎秒数回で済む）。エントリ表は
1 バンクぶん（768 バイト）だけキャッシュし、バンクが変わったときに読み直す。
裏はすべて内蔵フラッシュの XIP なので、1 回の読み出しは数 µs で終わる。

ファイル名は MDX ヘッダの PDX 名に `.PDX` を付けた `/MDX/<名前>.PDX`。名前は
ファイルの中身なので信用せず、ディレクトリ区切りと制御文字を弾く。FatFs の LFN 照合は
大小を無視するので、実体が小文字（`bos.pdx`）でも当たる。

#### デコードとレート変換

MSM6258 の ADPCM は 1 ニブル 4bit で、**下位ニブルが先**。

```
delta = step/8 + step/4·b0 + step/2·b1 + step·b2   （符号は b3）
出力  = clamp(出力 + delta, -2048, 2047)            （12bit）
段番号 = clamp(段番号 + {-1,-1,-1,-1,2,4,6,8}[n & 7], 0, 48)
```

12bit の結果を 4bit 左シフトして 16bit フルスケールにする。これは PCM8 の
16bit PCM 形式と同じレベルで、仕様の「8bit PCM の D/A 変換レベルは 16bit PCM と同一」
とも一致する。16bit PCM 形式（mode 5）は BE の signed short をそのまま、
8bit PCM 形式（mode 6）は signed char を 8bit 左シフトする。

**レート変換に補間は要らない。** X68000 では ADPCM のクロックも OPM のクロックも
同じ発振器から作られる。本機でも同じ関係を保ち、ADPCM のレートを φM の分周として
定義すると、出力の φM/64 との比は φM の値によらず必ず整数になる。

| mode | レート | φM との関係 | 出力フレーム/サンプル |
| --- | --- | --- | --- |
| 0 | 3.90625 kHz | φM/1024 | 16 |
| 1 | 5.20833 kHz | φM/768 | 12 |
| 2 | 7.8125 kHz | φM/512 | 8 |
| 3 | 10.41667 kHz | φM/384 | 6 |
| 4 | 15.625 kHz | φM/256 | 4 |
| 5 (16bit PCM) | 15.625 kHz | φM/256 | 4 |
| 6 (8bit PCM) | 15.625 kHz | φM/256 | 4 |

`mdx_play()` は φM を X68000 と同じ 4MHz へ寄せる（[§2.3](#23-実行時の切り替え)）ので、
実レートも実機と一致する。φM がそれ以外でも比は整数のままで、音程もテンポも ADPCM も
一緒にずれる（`mdx_play()` がその旨を出す）。

したがってソースサンプルをそのまま整数回繰り返すだけでよく、位相の小数部も除算も
要らない。これは MSM6258 の出力（階段波）そのものでもある。

#### ミックスリングと 2 本のカーソル

キャプチャの読み出しカーソルは USB と I2S で独立していて、位置も 1 回に読む長さも違う
（[§5](#5-i2s-出力)）。そこで ADPCM 側は **フレーム番号で引ける 16KB のリング**に
描いておき、読み出しでは消費しない。`ym3012_reader_read_pcm()` が渡してくる
`rd->read_total` は消費者によらない絶対フレーム番号なので、どちらのカーソルから
何フレーム単位で呼ばれても同じ結果を返せる（= 冪等）。リングの長さをキャプチャ側と
同じ 4096 フレームにしてあるのは、カーソルが書き込み位置から最大でも
リング 1 周ぶんしか離れないため。

加算は `ym3012_reader_read_pcm()` の変換直後の 1 箇所だけで行う
（[ym3012.c](../ym3012.c) の `ym3012_mixer_t` フック）。ここが USB キャプチャと
I2S の唯一の合流点なので、1 箇所で両方に効く。禁止コード E=0 の計数は混ぜる前に
済ませてあるので、統計は「取り込んだ信号の品質」のまま変わらない。

**飽和は 1 回だけ。** 8ch を int32 で足してからリングへ入れるときに 1 回、
FM に足すときにもう 1 回。チャンネルごとに 16bit へ飽和させると歪む。
ADPCM のフルスケールと FM のフルスケールを同じ重みで足すので、実機のアナログ
ミックスと同じく両方が大きいときは歪む。その回数は `s` の `CLIP` で見える。

#### まとめて描く理由

メインループは 20 万周/秒ほど回るのに対し、出力は 62500 フレーム/秒しかない。
毎周回描くと 1 周あたり 1 フレーム未満のためにブロックの支度をすることになり、
固定費だけで CPU の 2 割近くを使う。そこで **発音中は 64 フレーム（約 1ms）
溜めてから描く**。

まとめている途中のフレームを消費者が先に持って行くと、その分だけ混ぜ損ねて
ADPCM が途切れる。これを防ぐため `ym3012_set_mix_ready()` で
「描き終えたフレーム番号」を伝え、カーソルはそこを超えて読まない。I2S は
1024 フレームの先行を持っている（[§5.3](#53-出力リングと先行量の維持)）ので、
64 フレーム待たせても余裕には響かない。

無音のときは束ねない。リング全体がすでに 0 なので、書き直さずに位置だけ進める。

#### 鳴っているかを聴かずに確かめる

ADPCM が曲の終盤にしか出てこないことがある（`DS19P.MDX` は 12 秒の曲で ADPCM の
発音が 2 回だけ、しかも 6.5 秒過ぎから）。「聞こえない」だけでは曲の側の話か
ファームの側の話か分からないので、`mdx pcm` に発音を開始した回数（`keyon`）と
鳴らせなかった回数（`miss`）を出している。`keyon` が 0 のままなら曲がまだ
鳴らそうとしていない、`miss` が増えていれば PDX にその波形が無い（または音量 0）。

`mdx pcm off` は電源を切るまで残るので、off のまま PDX を要求する曲を再生したときは
`mdx play` が理由を出す。

#### 長時間止まったとき

フラッシュの消去などでメインループがリング 1 周（65.5ms）を超えて止まると、
描いても誰も読まない領域を延々と埋めることになる。遅れがリング 1 周を超えたら
リングを無音で埋めて位置を張り直す。なお通常の復帰では
`capture_resync_after_blackout()` が `pcm8_resync()` まで呼ぶので、この保険には
入らない（[§9.3](#93-書き込み中の停止とリング位置)）。

### 10.8 既存機能との共存

VGM と MDX は相互排他で、同時には鳴らせない。排他は中央のアービタではなく
`vgm_play()` / `mdx_play()` が直接相手を止めることで作る。どちらも、自分と相手のうち
`STOPPED` でない方を `vgm_stop()` / `mdx_stop()` で止めてからファイルを開く。

**止めるのを先にするのは、新しい曲の読み込み先が再生中の曲のデータそのものだから。**
MDX は `s_file`（64KB）が 1 面しかなく、VGM もストリーム用の `FIL` が 1 個しかない。
新旧を並べて持てない以上、読み込みに失敗しても前の曲へは戻せないので、そのまま停止状態で
エラーを返す仕様にしてある。ファイル名の書式検査だけは副作用が無いので停止より前に置き、
`bad argument` では前の曲を止めない。

`MDX_ENABLED=0` のスタブは `mdx_stop()` が hint と `unsupported` を返すので、`vgm.c` からは
`mdx_state() != MDX_STATE_STOPPED` で囲って呼ぶ（スタブの `mdx_state()` は常に
`MDX_STATE_STOPPED` なので、無効ビルドでは呼ばれない）。`mdx.c` から `vgm_stop()` を呼ぶ側は
VGM に無効化ビルドが無いので囲わない。

`ERROR` も止める対象に入れて `STOPPED` へ正規化する。`mdx_service()` は `mdx_fail()` で
`ERROR` に落ちた時点で `vgm_service()` と対称にキーオフと `pcm8_close_pdx()` を行うので、
`ERROR` のまま放置しても PDX が開いたままにはならない。

遅れの統計は `stats_seq_lag_*` を 2 つで共用する（同時に走らないので 1 個で足りる）。
曲を切り替えても 0 には戻らないので、曲ごとに見るなら `s 0`。

[§9.3](#93-書き込み中の停止とリング位置) の「書き込み中の停止とリング位置」の制約は
MDX にもそのまま掛かる。MDX はファイルを丸ごと RAM に載せているのでアンマウントされても
読めるが、**PDX は再生中もファイルを開いたまま読み続ける**（[§10.7](#107-adpcm-pcm8-の再生)）。
どちらにせよフラッシュの消去中は数十 ms 止まるので `storage host` は再生中拒否する。
`mdx stop` と曲の終わりで PDX は閉じる。


## 11. 自動連続再生 (autoplay)

[autoplay.c](../autoplay.c) は「曲が終わったら次を鳴らす」だけの薄い層で、**音を出す仕事は
一切しない。** VGM と MDX の再生系はそのままで、こちらは状態を見て `vgm_play()` /
`mdx_play()` を呼ぶ。ループを持つ曲は [vgm.c](../vgm.c) / [mdx.c](../mdx.c) だけでは
止まらない（どちらもループ回数を数えるだけ）ので、**打ち切りの判断だけがここにある。**

利用者側の仕様は [README §3.17](../README.md#317-autoplay自動再生)。

### 11.1 プレイリスト

名前は `'\0'` 区切りでプールへ詰め、`filelist_collect()`（[§1.3](#13-ファイル一覧の共用)）
が名前の昇順に並べたオフセット配列を持つ。

```c
static char     s_pool[AUTOPLAY_POOL_BYTES];    // 12KiB
static uint16_t s_offs[AUTOPLAY_MAX_ENTRIES];   // 名前の昇順。VGM の後ろに MDX
static uint16_t s_order[AUTOPLAY_MAX_ENTRIES];  // 鳴らす順。値は s_offs の添字
static uint32_t s_vgm_count;                    // s_offs の [0, s_vgm_count) が VGM
```

**曲の種別を持つ配列は無い。** `vgm_collect()` を先に、`mdx_collect()` を後に呼んで
同じバッファへ追記するので、`s_offs` 上の位置が `s_vgm_count` 未満かどうかがそのまま
「VGM か MDX か」になる。

`s_order` を `s_offs` と分けてあるのは、シャッフルしても「一覧としての並び」を壊さない
ため。`list` では恒等写像、`random` では Fisher-Yates（`get_rand_32()`）で並べ替える。
前方へ一巡したところで並べ直し、**並べ直した直後の先頭が直前に鳴らした曲だったら 1 個
ずらす**（切れ目で同じ曲が 2 回続かないように）。

プレイリストを作るのは `autoplay start` のときだけ。再生中にディレクトリを走査し直すと、
曲の途中で数十 ms 止まるうえ、鳴らしている曲の位置を見失う。

### 11.2 状態機械

`autoplay_service()` は [§1.1](#11-メインループ) のシーケンサ群の**後ろ**で回す。同じ周回で
更新された `vgm_state()` / `mdx_state()` をそのまま見られる。

| 状態 | 抜ける条件 | 次 |
| --- | --- | --- |
| `PLAYING` | 再生系が `PLAYING` でなくなった（終端 / `ERROR`） | `GAP` |
| `PLAYING` | `loop != 0` かつループカウンタが `loop` に達した | `FADING`（`fade == 0` なら `GAP`） |
| `FADING` | フェードの期限が来た、または曲が先に終わった | `GAP` |
| `GAP` | 無音の期限が来た | 次の曲を鳴らして `PLAYING` |

**終わりの検出はポーリングしかできない。** 再生系にコールバックの口は無く、終了は
`vgm_service()` / `mdx_service()` が状態を落として 1 行出すだけ。`ERROR` は `STOPPED` に
ならず残るので、`*_stop()` を呼んで `STOPPED` へ正規化してから次へ送る。

**曲名は autoplay 側が持つ。** `vgm_current_name()` は停止した瞬間に空文字列を返すので、
「いま何番目の何を鳴らしていたか」は再生系からは引けない。

`start_track()` は鳴らせる曲が見つかるまで送り、1 曲ずつ理由を出す。プレイリストを
一巡しても 1 曲も鳴らせなければ自動再生ごと止める（`p 1` 中に φM の違う曲へ進むと
`clockmode_follow_file()` が拒否するので、この経路は普通に通る）。

`s_busy` は `autoplay_service()` を抑止するフラグ。`vgm list` / `mdx list` /
`autoplay list` は 1 行ごとに `service_all()` を tick として呼ぶので、一覧を出している
最中にここから曲送りが走りうる。プレイリストを作る間と出す間は動かさない。

### 11.3 フェードアウト

`ym3012_reader_read_pcm()` の**ミキサ適用後**に掛けるデジタルゲインで作る
（[§4.3](#43-pcm-への変換) の合流点）。ADPCM を混ぜたあとなので FM と ADPCM の両方が
一緒に落ち、I2S と USB キャプチャの両方に同じように効く。

**ゲインは絶対フレーム番号の関数にしてある。** カーソルは I2S とキャプチャで 2 本あり
読み出し位置も読む量も違うので、経過時刻で決めると 2 本で違う音になる。ミキサフックが
`first_frame` を受けているのと同じ理屈。

```
残り比 q = 1 - (frame - start) / frames      (Q16)
ゲイン   = q * q                              (線形だと終わり際で急に消えて聞こえる)
```

除算はフレームごとには行わない。開始時に `1/frames` を Q32 で 1 回だけ求め、以後は
乗算とシフトで引く。

**解除もフレーム番号で持つ。** ゲインを即座に 1.0 へ戻すと、リングにまだ残っている
「フェード済みのはずの区間」が全音量で読み出されて十数 ms のノイズになる。
`ym3012_fade_release()` は「このフレーム以降だけ 1.0 に戻す」境界を置くだけで、
それより前は落とし切ったままにする。

**境界を置く前に `ym3012_ring_poll()` を呼ぶ。** 取り込み位置 `s_write_total` は
`service_all()` の音声チェーン（500us 間隔、[§1.1](#11-メインループ)）でしか進まず、
`autoplay_service()` はその 1/10 の間隔で走る。さらに `key_off_all()` が約 1.3ms
ブロックする。引き直さないと境界が最大 1.8ms 過去に落ち、**まだ落とすべき区間まで
解除されて**しまう。位相が保たれていること（[§9.3](#93-書き込み中の停止とリング位置)）が
前提で、崩れているとこの引き直しをしても境界はその分だけ過去に落ちる。

**解除するのは停止（キーオフ）の後**で、`stop_playback()` が 2 つの `*_stop()` の
後ろで呼ぶ。順序が逆だと、まだ鳴っているチップの音が全音量で出てしまう。

**ただしキーオフの直後ではまだ黙っていない。** `key_off_all()` は全スロットの RR を
15 にしてから落とすが、それでもチップのリリースは残る。実測で **KC=0 / KS=0（リリースが
最も遅くなる条件）でも 0 に達するまで約 5.5ms**。ここを詰めると、その区間が全音量で
出て「プチッ」と鳴る。そこで `AUTOPLAY_RELEASE_MS`（16ms）の猶予を置いてから、
`AUTOPLAY_RELEASE_RAMP_MS`（4ms）のランプで 1.0 へ戻す。猶予はフェードが 0 に達した
あとの無音を延ばすだけなのでコストが無く、余裕を 3 倍取ってある。ランプは見積もりが
外れて音が残っていたときの保険。カーブはフェードアウトの `q²` を時間で反転したもの。

`key_off_all()` は**全 32 スロットの RR を書いてから、キーオフ 8 回をまとめて撃つ**。
ch ごとに交互に書くと最初と最後のチャンネルでキーオフが 1.3ms ずれ、リリースの残りも
同じだけ滲むので、猶予の見積もりが立たなくなる。

**境界は前へしか動かない。** `ym3012_fade_start()` で `UINT64_MAX` に初期化されて
1 回のフェードのあいだ単調減少し、後ろへ動かす要求は無視する。一度解除した区間を
無音に塗り直さないため。この規則があるので、`start_track()` が曲を始めた時点で
`ym3012_fade_release(0, ramp)` を打ち直せる。`GAP` を挟まない `autoplay next` / `prev` や
`autoplay gap 0` では猶予の大半が新しい曲の頭に被るので、この打ち直しは省略できない
（猶予が済んでいれば何もしない）。

**基板の YM3012 のアナログ出力には効かない。** Pico は OPM から DAC へのシリアル線を
傍受しているだけで、その経路には入っていない。アナログ側で音が止まるのは、フェードが
終わったあとのキーオフの時点になる。

MDX は MXDRV 由来の TL フェード（`0xE7 0x01`）を別に持っているが、autoplay はそちらを
使わない。VGM に相当物が無く、曲の種類でフェードの効く出力先が変わってしまうため。

### 11.4 他のモジュールとの関係

- **手動の `vgm play` / `mdx play` / `*_stop` はコマンド層で `autoplay_stop()` を先に呼ぶ。**
  autoplay 自身も `vgm_play()` を呼ぶので、区別はコマンド層に置くのが一番単純になる。
- **`storage host` は `autoplay_is_running()` でも拒否する**（[§8.4](#84-所有権の切り替え)）。
  曲間（`GAP`）は VGM も MDX も鳴っておらず、既存の 2 つの判定では素通りしてしまう。
- 統計（`STATS_SVC_*`）は足していない。`autoplay_service()` は比較が数回で、`s` の
  `SVC` 行を増やすほどの滞在時間にならない。
