# 同梱物のライセンス

このパッケージに入っている `pico-opm-writer.uf2` は、本プロジェクトのコードに加えて
下記の第三者コードをリンクした結果である。それぞれのライセンス本文を同梱している。

pico-opm-writer 自身のライセンスは MIT。[LICENSE](LICENSE) を参照。

| コンポーネント | 版 | ライセンス | 本文 |
| --- | --- | --- | --- |
| Raspberry Pi Pico SDK（Raspberry Pi Ltd.） | 2.3.0 | BSD-3-Clause | [licenses/pico-sdk-LICENSE.txt](licenses/pico-sdk-LICENSE.txt) |
| TinyUSB（hathach, tinyusb.org） | Pico SDK 同梱版 | MIT | [licenses/tinyusb-LICENSE](licenses/tinyusb-LICENSE) |
| FatFs（ChaN） | R0.16 | 1 条項の BSD 風 | [external/fatfs/LICENSE.txt](external/fatfs/LICENSE.txt) |
| miniz（Rich Geldreich ほか） | 3.1.2 | MIT | [external/miniz/LICENSE](external/miniz/LICENSE) |

FatFs と miniz の入手元・適用したパッチ・設定は [external/README.md](external/README.md) にある。

`pico_sdk_import.cmake`（ビルドスクリプト。バイナリには含まれない）も
Raspberry Pi (Trading) Ltd. 由来で BSD-3-Clause が適用される。

## ソースを含まない準拠先

MDX の解釈は X68000 の音源ドライバ MXDRV
（(c)1988-92 milk., K.MAEKAWA, Missy.M, Yatsube）の仕様に準拠している。
準拠先は大半が **X68k MXDRV music driver version 2.06+17 Rel.X5-S /
for Win32 [MXDRVg] V2.00b** で、一部の機能だけ **MXDRV 2.06+16 Rel.3+25** に準拠する。

ADPCM パートは **PCM8 version 0.48**（(c) 江藤啓 1991,92）の技術資料に準拠している。

どちらもソース・バイナリ・資料とも本プロジェクトには含まれておらず、
`mdx.c` と `pcm8.c` は独自に書き起こしたものである。

## ソース

https://github.com/hyano/pico-opm-writer
