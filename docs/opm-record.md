# opm-record

[tools/opm-record.py](../tools/opm-record.py)

pico-opm-writer に入っている曲を 1 曲 1 ファイルで WAV に録るドライバ。曲の長さを
事前に知る必要はない — 録り始めも録り終わりもファーム側が決める
（[README §3.10](../README.md#310-ppcm-出力) の `p 2` と
[README §3.22](../README.md#322-曲の終わり方)）。

シリアルの扱いと WAV の書き出しは [opm-writer.py](opm-writer.md) に任せ、曲ごとに
小さなシーケンスファイルを作ってサブプロセスで起動する。

外部ライブラリは使わない（標準ライブラリのみ）。

## 使い方

```bash
# 曲を並べて録る
./tools/opm-record.py --loop 2 -o out/ VGM:GRADIUS.VGM MDX:SORCER/OP.MDX

# 実機に入っている曲の一覧を出す（録音はしない）
./tools/opm-record.py --list
./tools/opm-record.py --list --source mdx

# 実機に入っている曲を全部録る
./tools/opm-record.py --all --loop 2 -o out/

# 実機に触らず、実行内容だけ確認する
./tools/opm-record.py -n --loop 2 -o out/ VGM:GRADIUS.VGM
```

## 曲の指定

位置引数で指定する。パスは `/VGM/` `/MDX/` からの相対パスで、サブフォルダは `/` で
区切る（[README §3.14](../README.md#314-vgmvgm-再生)）。

| 書き方 | 種別の決まり方 |
| --- | --- |
| `VGM:<path>` / `MDX:<path>` | 接頭辞で決める。大小は区別しない |
| `<path>` | 拡張子で決める。`.vgm` / `.vgz` は VGM、`.mdx` は MDX |

`:` は FAT のファイル名に使えないので、含まれていれば必ず種別の接頭辞として扱う。
接頭辞が `VGM:` でも `MDX:` でもなければエラー。拡張子でも決まらない名前もエラーで、
どちらの場合も実機には触れずに終了コード 1 で終わる。

曲名に空白を含んでいてもそのまま書ける（シェルのクォートは必要）。

```bash
./tools/opm-record.py -o out/ "VGM:THX/02 Coin.vgz"
```

## 一覧の取得

`--list` は `vgm list` / `mdx list` を実機に投げ、結果を TSV で出す。`--source` で
どちらを見るか選ぶ（既定 `both`）。**録音はしない。**

```
kind	size	path
vgm	3531	THX/01 The Thunder Fighters (Title).vgz
mdx	937	BOSCONIAN/BOS01.MDX
```

| 列 | 内容 |
| --- | --- |
| `kind` | `vgm` / `mdx` |
| `size` | ファイルのバイト数（`.vgz` は圧縮された状態） |
| `path` | 各ディレクトリからの相対パス |

一部だけ録りたいときは、この出力を絞って位置引数に流し込む。

```bash
./tools/opm-record.py --list --source mdx | awk -F'\t' 'NR>1 && $3 ~ /^BOSCONIAN/ {print "MDX:" $3}' \
  | xargs -d '\n' ./tools/opm-record.py --loop 2 -o out/
```

`--all` は同じ一覧をそのまま録音対象にする。位置引数との併用はできない。

## 出力

1 曲 1 ファイル。相対パスの `/` を `_` に潰して `--out` のディレクトリへ 1 階層で並べ、
拡張子は `--format` で決まる。

| 曲 | 出力 |
| --- | --- |
| `MDX:SORCER/OP.MDX` | `out/SORCER_OP.wav.zst` |
| `VGM:GRADIUS.VGM` | `out/GRADIUS.wav.zst` |

WAV のサンプリングレートは**録音した時点で実機が使っていた値**が入る。VGM は
ファイル側のクロックを要求すると φM が切り替わるので、曲によって 62500Hz
（φM 4MHz）と 55930Hz（φM 3.579545MHz）が混ざる（[README §3.16](../README.md#316-clockクロック切り替え)）。

標準出力（行頭で種別が分かる）:

| 接頭辞 | 意味 |
| --- | --- |
| `--- [n/N] <曲>` | これから録る曲 |
| `\| ` | `opm-writer.py` の出力をそのまま中継したもの |
| `--- ` | 末尾のサマリ（曲数 / エラー件数） |

エラーは `! ` を付けて標準エラーへ出す。終了コードはエラー 0 件で 0、それ以外は 1。

## 引数

### 位置引数

| 引数 | 説明 |
| --- | --- |
| `SONG ...` | 録る曲。`--all` / `--list` とは併用できない |

### オプション

| オプション | 既定 | 説明 |
| --- | --- | --- |
| `-o`, `--out DIR` | カレント | 出力先ディレクトリ |
| `--all` | off | 実機に入っている曲を全部録る |
| `--list` | off | 一覧を TSV で出すだけ（録音しない） |
| `--source vgm\|mdx\|both` | `both` | `--all` / `--list` の対象 |
| `--loop N` | `2` | 何周でフェードアウトして終わるか。`0` で無限 |
| `--fade MS` | `2000` | フェードアウトの長さ [ms] |
| `--format wav.zst\|wav` | `wav.zst` | 出力形式 |
| `--phim PHIM` | `4000000` | φM [Hz]。実機から読めなかったときのフォールバック |
| `--device DEVICE` | 自動検出 | コマンド用 USB CDC のデバイス |
| `--pcm-device DEVICE` | 自動検出 | PCM 出力の USB CDC のデバイス |
| `--zstd-level N` | `22` | `.wav.zst` の圧縮レベル |
| `--song-max-ms MS` | `900000` | 1 曲に費やす上限 [ms]。超えたら失敗 |
| `--retry N` | `3` | 1 曲あたりのリトライ回数。`0` で無効 |
| `--retry-wait SEC` | `5` | リトライまでの待ち時間 [秒] |
| `-n`, `--dry-run` | off | シリアルに触らず、実行内容だけ表示する |
| `--self-test` | off | 実機なしで自己検証する |

**`--loop 0` は上限が効かなくなる。** ループを持つ曲は止まらないので、`--song-max-ms`
に達して失敗する。ループしない曲を録るときだけ意味がある。

## 依存

- Python 3（標準ライブラリのみ）
- 同じ `tools/` にある `opm-writer.py`（`--dry-run` でも必要）

`.wav.zst` 出力には Python 3.14 以上が必要（`compression.zstd` module）。
古い Python では `--format wav` を使う。

## 補足

### 生成するシーケンス

曲 1 本につき、一時ディレクトリへこれを書いて `opm-writer.py` に渡す。

```
<kind> loop <n>
<kind> fade <ms>
!capture-song <kind> play <path>
<kind> stop
```

`!capture-song` の中身は [docs/opm-writer.md](opm-writer.md) を参照。`--stop-on-error` を
必ず付けて起動するので、途中で失敗したら残りは実行されない。

### リトライ

`opm-writer.py` が失敗した曲は `--retry` 回まで録り直す。ホストが CDC #1 を読み
落とした場合と、φM の切り替えなど状態の都合で 1 回目が通らなかった場合を拾う。
使い切ったらその曲だけ諦めて次へ進み、最後にエラー件数を出す。

### 自己検証

`--self-test` は実機に触れずに、曲指定の解釈・出力名・生成するシーケンス・一覧の
解析を検証する。全ケース `PASS` で終了コード 0。

```bash
./tools/opm-record.py --self-test
```
