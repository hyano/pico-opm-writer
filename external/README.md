# external/

外部のソースコードをそのまま置く場所。**ここのファイルは改変しない。**

自前のコードはリポジトリ直下に置く（`ffconf.h` / `flash_disk.c` / `diskio_flash.c` など）。

## fatfs — FatFs R0.16

汎用 FAT ファイルシステムモジュール。作者 ChaN。ライセンスは [fatfs/LICENSE.txt](fatfs/LICENSE.txt)
（1 条項の BSD 風。著作権表示を残せば商用利用も再配布も可）。

| 項目 | 値 |
| --- | --- |
| 版 | R0.16（2025-07-22） |
| Revision ID (`FF_DEFINED`) | 80386 |
| 取得元 | https://elm-chan.org/fsw/ff/arc/ff16.zip |
| SHA256 | `99f7dc1f7e095356e4a9e3dbe29959090d8b948afe2bbc5441e52fdf4b85449e` |

### 置いてあるファイル

`ff16.zip` の `source/` から本体だけを取り、`LICENSE.txt` はアーカイブ直下から取っている。

```
ff.c  ff.h  ffunicode.c  diskio.h  00readme.txt  00history.txt  LICENSE.txt
```

### 意図的に置いていないファイル

| ファイル | 理由 |
| --- | --- |
| `ffconf.h` | 上流のものは設定テンプレート。プロジェクト側の設定を使うため**ここに置いてはいけない**（下記） |
| `ffsystem.c` | `ff_memalloc` / `ff_mutex_*` の実装。`FF_USE_LFN == 3` と `FF_FS_REENTRANT` でしか呼ばれず、本プロジェクトはどちらも使わない |
| `diskio.c` | 上流のものは配布物ではなくサンプル。本プロジェクトは `diskio_flash.c` で実装する |

### `ffconf.h` をプロジェクト側に置く仕組み

`ff.h` の `#include "ffconf.h"` がツリー内で唯一の参照（`ff.c` は `ff.h` と `diskio.h` しか
include しない）。GCC は `"..."` を**まず include 元のファイルがあるディレクトリ**から探すため、
`external/fatfs/` に `ffconf.h` が無ければ、`-I` に入っているリポジトリ直下の `ffconf.h` が拾われる。

**ここに `ffconf.h` を置くと黙って上流のテンプレートが勝つ。** 事故防止として
リポジトリ直下の `ffconf.h` は `OPM_FFCONF_H` を定義し、`diskio_flash.c` がそれを
`#error` で検査している。

`ff.h` は `FF_DEFINED != FFCONF_DEF` をコンパイル時に弾くので、版と設定のずれも検出される。

### 適用した公式パッチ

https://elm-chan.org/fsw/ff/patches.html のものを、この順で適用している。

| 順 | パッチ | 日付 | 対象 | 内容 |
| --- | --- | --- | --- | --- |
| 1 | `ffunicode.zip` | 2025-08-03 | `ffunicode.c` を丸ごと差し替え | 変換テーブルの圧縮改善 |
| 2 | `ff16p1.diff` | 2025-09-13 | `ff.c` | `f_mkfs` の最小ボリュームを 128 → 64 セクタへ |
| 3 | `ff16p2.diff` | 2026-07-10 | `ff.c` | セキュリティ修正（CVE-2026-6682 / 6683 / 6687 ほか） |

SHA256 と検証手順は、パッチを適用したコミットのメッセージに記録してある。

### 更新するとき

1. 新しいアーカイブを展開し、上記 7 ファイルだけを差し替えて `diff` で一致を確認する
2. `ffconf.h` を新しい版のテンプレートと突き合わせ、増減したオプションを反映する
   （`FFCONF_DEF` は新しい `ff.h` の `FF_DEFINED` に合わせる）
3. その版に対応する公式パッチを当て直す
4. この表を書き換える

素の vendoring とパッチ適用は**コミットを分ける**。上流そのままとの差分が
git 上で 1 コミットとして読めるようにするため。

## miniz — miniz 3.1.2

zlib / DEFLATE 互換の圧縮・展開ライブラリ。作者 Rich Geldreich ほか。
ライセンスは [miniz/LICENSE](miniz/LICENSE)（MIT。本プロジェクトと同一）。

`.vgz`（gzip 圧縮された VGM）のストリーム展開に、展開器 `tinfl` だけを使う。

| 項目 | 値 |
| --- | --- |
| 版 | 3.1.2（2026-07-01） |
| 取得元 | https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip |
| SHA256 | `f0446d863f9c19926ad9483c523fdc42e42b8d4a6a431d27e09d49c79a140d9a` |

### 置いてあるファイル

配布物は amalgamated（1 対のソースとヘッダ）なので、そのまま置く。

```
miniz.c  miniz.h  LICENSE  readme.md  ChangeLog.md
```

`miniz.h` は `miniz_export.h` を include せず、`MINIZ_EXPORT` が未定義なら空に
展開する。CMake で生成するヘッダは要らない。

### 意図的に置いていないファイル

| ファイル | 理由 |
| --- | --- |
| `examples/` | 使用例。ホスト向けで `stdio` を使う |

### 設定

FatFs と違い設定ヘッダを持たないので、`CMakeLists.txt` の `miniz` ターゲットで
マクロを定義する。圧縮側と zip アーカイブ側を全部落とし、`tinfl` だけを残す。

| マクロ | 意図 |
| --- | --- |
| `MINIZ_NO_STDIO` | `fopen` 等を使わせない |
| `MINIZ_NO_TIME` | `time.h` を使わせない |
| `MINIZ_NO_MALLOC` | ヒープを使わせない（`tinfl_decompress` は元々使わない） |
| `MINIZ_NO_DEFLATE_APIS` | 圧縮は不要 |
| `MINIZ_NO_ARCHIVE_APIS` | zip 読み書きは不要 |
| `MINIZ_NO_ZLIB_APIS` | `mz_stream` 系は不要。使うのは `tinfl_decompress` だけ |

`MINIZ_NO_ARCHIVE_WRITING_APIS` は指定しない。`MINIZ_NO_DEFLATE_APIS` から
`miniz.h` が自分で定義するので、外から渡すと再定義の警告になる。

`MINIZ_LITTLE_ENDIAN` / `MINIZ_USE_UNALIGNED_LOADS_AND_STORES` /
`MINIZ_HAS_64BIT_REGISTERS` は上書きしない。`miniz.h` が `__BYTE_ORDER__` と
CPU 判定から Arm 向けに `1 / 0 / 0` を選ぶので、そのままで正しい。

### 更新するとき

1. 新しいアーカイブを展開し、上記 5 ファイルだけを差し替えて `diff` で一致を確認する
2. `tinfl_decompressor` の構造体にポインタが増えていないことを確認する
   （`vgz.c` はこの構造体を `memcpy` で保存・復元する。ポインタが入ると壊れる）
3. `MINIZ_NO_*` の名前が変わっていないか `miniz.h` の冒頭コメントで確認する
4. この表を書き換える
