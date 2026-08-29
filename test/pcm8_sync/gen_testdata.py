#!/usr/bin/env python3
"""
FM と ADPCM の発音タイミングを比べるための MDX / PDX を生成する。

[README.md](README.md) §4 の「解析方法」で使う一次データの作成器。実機には触らない。

## 何を作るか

FM 1 本と ADPCM 1 本が **同じ tick で同時にキーオンする**だけの曲を、2 条件ぶん作る。
両者を左右に振り分けてあるので、1 回のキャプチャで立ち上がりを直接比べられる。

| ファイル | 条件 | 何が変わるか |
| --- | --- | --- |
| `SYNC.MDX` | quiet | 音と音のあいだ、ADPCM は 1ch も鳴っていない |
| `SYNC2.MDX` | busy | ほぼ無音のドローンを ADPCM ch9 で鳴らし続ける |

2 条件が要るのは、[pcm8.c](../../src/pcm8.c) の `pcm8_service()` が
**発音中だけ 64 フレーム束ねて描く**（`s_sounding && behind < PCM8_BATCH_FRAMES`）ため。
quiet では束ねが働かず、busy では働く。ドローンは `active` を立て続けるためだけのもので、
振幅は解析の閾値のはるか下に置く（[analyze_sync.py](analyze_sync.py) の `--threshold`）。

| チャンネル | 定位 | 音 |
| --- | --- | --- |
| FM ch0 | 右（`p2` = OPM `RL` の bit7） | 単一オペレータの正弦波。AR=31 で瞬時に立ち上がる |
| ADPCM ch8 | 左（`p1`。PCM8 の定位は `pan & 1` で L に出る） | 8bit PCM 形式（mode 6）の直流。**1 ソースサンプルで全振幅**になる |
| ADPCM ch9 | 左（busy 条件のみ） | 振幅 1 の直流。最小音量で鳴らすので出力は 40 前後にしかならない |

**PCM8 の定位は全チャネル共通**なので、ドローンも `p1` にしないと `s_pan` が
書き換わって ADPCM が右へも出てしまう。

分離できる理由は 2 つ。

- FM は OPM のレジスタ `0x20+ch` の `RL` で片側だけに出す。反対側のスロットには
  何も加算されない。
- ADPCM は [pcm8.c](../../src/pcm8.c) の `render_block()` が `pan & 1` で L、`pan & 2` で R に
  振り分ける。ミックスリングの段階で片側は 0 のままになる。

**波形を鋭くしてある**のは、立ち上がりのフレームを 1 フレーム単位で決めたいため。
ADPCM に ADPCM 形式（mode 0-4）を使うと、MSM6258 のステップ幅が小さい値から始まるので
全振幅まで 1ms ほどかかり、測りたい量（最大 93 フレーム = 1.5ms）と同じ桁になってしまう。
8bit PCM 形式なら 1 ソースサンプル = 4 出力フレームで全振幅に達する。

## 書式

MDX のヘッダは [docs §10.2](../../docs/pico-opm-writer.md#102-データ構造とローダ)、
PDX のエントリ表は [pcm8.h](../../src/pcm8.h) の `PDX_*` の定義に従う。どちらも
ビッグエンディアン。

## 使い方

```bash
./gen_testdata.py                 # SYNC.MDX / SYNC2.MDX / SYNC.PDX をこのディレクトリに作る
./gen_testdata.py -o /tmp         # 出力先を指定する
./gen_testdata.py --self-test     # 生成物の自己検証（実機不要）
```

作った 2 つのファイルは、ファーム側を `storage host` にして USB マスストレージの
`/MDX/` へコピーする（再生中は `storage host` が拒否されるので停止状態で行う）。

SPDX-License-Identifier: MIT
"""

import argparse
import os
import struct
import sys

# ---- 曲のパラメータ ------------------------------------------------------

# Timer-B。1 clock = 1024 * (256 - TEMPO) φM サイクル。
# 240 なら 16384 サイクル = φM 4MHz で 4.096ms。
TEMPO = 240

# 1 音の長さ（clock）。この間隔で立ち上がりが繰り返す。
NOTE_CLOCKS = 6

# ゲートタイム。q = 2 なら鳴っているのは (2 * 長さ) >> 3 clock だけで、
# 残りは無音になる。無音を挟まないと次の立ち上がりが見つけられない。
GATE_Q = 2

# FM の音高（MDX の音符番号。0 = o0 d+）。高いほど 1 周期が短く、
# 立ち上がりの検出が鋭くなる。84 は約 2.5kHz。
FM_NOTE = 84

# ADPCM の音高 = PDX のエントリ番号。
PCM_NOTE = 0

# PDX の波形。8bit signed PCM を 15.625kHz で 96 サンプル = 6.14ms。
# ゲートタイム (2 clock = 8.19ms) より短いので、キーオフではなくデータの尽きで終わる。
PCM_LEVEL = 0x60
PCM_SAMPLES = 96

# PCM8 の音量。@v の値 21 は PCM_VOLUME_TBL[21] = 8 = 原音量（利得 1.0）。
# ここを上げると 8bit PCM の全振幅と掛かって飽和する。
PCM_VOL_AT = 21

# busy 条件のドローン。PDX のエントリ 1。振幅 1 を最小音量 (@v 42 -> PCM8 の音量 0 =
# 利得 0.158) で鳴らすので、出力は 40 前後にしかならない。
# 長さは 1 音の間隔 (6 clock = 24.58ms) より十分長くして、データが尽きないようにする。
DRONE_NOTE = 1
DRONE_LEVEL = 0x01
DRONE_SAMPLES = 4096
DRONE_VOL_AT = 42
DRONE_MDX_CH = 9

# ドローンのゲートタイム。8 なら鳴っている長さ = 音の長さになるので、キーオフと
# 次のキーオンが同じ tick に来て、レンダラから見た `active` が途切れない。
# 測定側の GATE_Q (= 2) を使うと 1/3 の時間しか鳴らず、束ねが働く区間ができない。
DRONE_GATE_Q = 8

TITLE = "PCM8 SYNC TEST"
PDX_NAME = "SYNC.PDX"

# ---- MDX のオペコード ----------------------------------------------------

OP_TEMPO = 0xFF  # @t <t>
OP_VOICE = 0xFD  # @ <n>（ADPCM では PDX のバンク番号）
OP_PAN = 0xFC  # p <n>
OP_VOL = 0xFB  # v <n> / @v <n>（bit7 で区別）
OP_GATE = 0xF8  # q <n>
OP_LOOP = 0xF1  # 演奏終了（次が 0x00）/ ループ（次が 2 バイトの相対位置）
OP_NOISE = 0xED  # FM はノイズ周波数、ADPCM はサンプリング周波数
OP_PCM8 = 0xE8  # PCM8 モード宣言

MDX_CH_COUNT = 16  # FM 8 + ADPCM 8（PCM8 拡張）
MDX_FM_CH = 8

PDX_BANK_NOTES = 96
PDX_ENTRY_BYTES = 8
PDX_BANK_BYTES = PDX_BANK_NOTES * PDX_ENTRY_BYTES


def note_op(n):
    """音符のオペコード。0x80 + 音符番号（0-95）。"""
    if not 0 <= n <= 95:
        raise ValueError("note out of range: %d" % n)
    return 0x80 + n


def loop_to(here, target):
    """
    `here` にある 0xF1 から `target` へ戻るループを組む。

    ファーム側は 2 バイトのワードを読んだあとの位置から `~w + 1` だけ戻る
    （[mdx.c](../../src/mdx.c) の `case 0xF1u`）。ワードの上位バイトが 0 になると
    「演奏終了」と区別が付かなくなるので、戻り幅が 0x0100 未満であることも確かめる。
    """
    after = here + 3  # 0xF1 と 2 バイトのワードを読み終えた位置
    back = after - target
    if back <= 0:
        raise ValueError("loop must go backwards")
    w = (-back) & 0xFFFF
    if (w >> 8) == 0x00:
        raise ValueError("loop word would look like an end marker")
    return bytes([OP_LOOP, w >> 8, w & 0xFF])


# ---- 音色 ----------------------------------------------------------------


def make_voice(number):
    """
    27 バイトの音色レコードを作る。並びは
    番号 / FL・CON / スロットマスク / DT1・MUL x4 / TL x4 / KS・AR x4 /
    AME・D1R x4 / DT2・D2R x4 / D1L・RR x4（[mdx.c](../../src/mdx.c) の `snd_voice()`）。
    オペレータの順は M1 / M2 / C1 / C2。

    CON=7（4 オペレータすべてキャリア）にして、C2 だけ TL=0 で鳴らし残り 3 個は
    TL=127 で黙らせる。1 オペレータの素の正弦波にしておくと、立ち上がりが
    エンベロープだけで決まって読みやすい。
    """
    v = bytearray()
    v.append(number)
    v.append(0x07)  # FL=0 CON=7
    v.append(0x0F)  # キーオンのスロットマスク（4 個すべて）
    v += bytes([0x01, 0x01, 0x01, 0x01])  # DT1=0 MUL=1
    v += bytes([0x7F, 0x7F, 0x7F, 0x00])  # TL: C2 だけ 0
    v += bytes([0x1F, 0x1F, 0x1F, 0x1F])  # KS=0 AR=31（瞬時に立ち上がる）
    v += bytes([0x00, 0x00, 0x00, 0x00])  # AME=0 D1R=0
    v += bytes([0x00, 0x00, 0x00, 0x00])  # DT2=0 D2R=0
    v += bytes([0x0F, 0x0F, 0x0F, 0x0F])  # D1L=0 RR=15（瞬時に切れる）
    assert len(v) == 27
    return bytes(v)


# ---- MML -----------------------------------------------------------------


def fm_mml(base_at):
    """FM ch0 の MML。`base_at` はこの列が置かれる絶対位置。"""
    head = bytes(
        [
            OP_TEMPO, TEMPO,
            OP_PCM8,               # ADPCM を PCM8 経路で鳴らす（IOCS 経路にしない）
            OP_PAN, 0x02,          # p2 = 右のみ
            OP_GATE, GATE_Q,
            OP_VOICE, 0x00,        # @0
            OP_VOL, 0x80,          # @v0 = TL の加算 0（最大音量）
        ]
    )
    body = bytes([note_op(FM_NOTE), NOTE_CLOCKS - 1])
    at = base_at + len(head)  # ループの戻り先 = 音符の先頭
    return head + body + loop_to(base_at + len(head) + len(body), at)


def pcm_mml(base_at):
    """ADPCM ch8 の MML。"""
    head = bytes(
        [
            OP_PCM8,               # ch0 より先に読まれても PCM8 経路になるよう再宣言する
            OP_NOISE, 0x06,        # ADPCM のサンプリング周波数 = mode 6（8bit PCM）
            OP_PAN, 0x01,          # p1 = 左のみ
            OP_GATE, GATE_Q,
            OP_VOICE, 0x00,        # PDX のバンク 0
            OP_VOL, 0x80 | PCM_VOL_AT,
        ]
    )
    body = bytes([note_op(PCM_NOTE), NOTE_CLOCKS - 1])
    at = base_at + len(head)
    return head + body + loop_to(base_at + len(head) + len(body), at)


def drone_mml(base_at):
    """
    ADPCM ch9 のドローン。`pcm8_service()` の `s_sounding` を立てたままにするためだけの
    もので、音として聞かせる意図は無い。

    **定位は ch8 と同じ左にする。** PCM8 の定位は全チャネル共通なので、ここで
    `p1` 以外を出すと `s_pan` が書き換わって ADPCM が右へも漏れる。
    """
    head = bytes(
        [
            OP_PCM8,
            OP_NOISE, 0x06,        # 8bit PCM
            OP_PAN, 0x01,          # p1 = 左のみ（ch8 と同じにする）
            OP_GATE, DRONE_GATE_Q,
            OP_VOICE, 0x00,
            OP_VOL, 0x80 | DRONE_VOL_AT,
        ]
    )
    body = bytes([note_op(DRONE_NOTE), NOTE_CLOCKS - 1])
    at = base_at + len(head)
    return head + body + loop_to(base_at + len(head) + len(body), at)


def idle_mml():
    """使わないチャンネル。すぐ演奏終了させる。"""
    return bytes([OP_LOOP, 0x00])


# ---- MDX -----------------------------------------------------------------


def build_mdx(busy=False):
    header = TITLE.encode("shift_jis") + b"\x0d\x0a\x1a" + PDX_NAME.encode("ascii") + b"\x00"
    base = len(header)

    # 表は base から 2 + 2*N バイト。ファーム側はチャンネル数を
    # 「先頭チャンネルのオフセット」から逆算するので、ch0 の MML は表の直後に置く。
    table_bytes = 2 + 2 * MDX_CH_COUNT

    mml = [None] * MDX_CH_COUNT
    offs = [0] * MDX_CH_COUNT
    at = table_bytes  # base 相対

    for i in range(MDX_CH_COUNT):
        offs[i] = at
        if i == 0:
            body = fm_mml(base + at)
        elif i == MDX_FM_CH:
            body = pcm_mml(base + at)
        elif busy and i == DRONE_MDX_CH:
            body = drone_mml(base + at)
        else:
            body = idle_mml()
        mml[i] = body
        at += len(body)

    voice_off = at
    voices = make_voice(0)

    table = struct.pack(">H", voice_off) + b"".join(struct.pack(">H", o) for o in offs)
    assert len(table) == table_bytes

    return header + table + b"".join(mml) + voices


# ---- PDX -----------------------------------------------------------------


def build_pdx():
    """
    バンク 1 面ぶんのエントリ表（96 音 x 8 バイト = BE のオフセットと長さ）に
    波形を続けたもの。長さ 0 のエントリは「波形なし」として扱われる。

    エントリ 0 が測定に使う直流、エントリ 1 が busy 条件のドローン。
    """
    table = bytearray(PDX_BANK_BYTES)
    data = bytearray()

    for note, level, count in (
        (PCM_NOTE, PCM_LEVEL, PCM_SAMPLES),
        (DRONE_NOTE, DRONE_LEVEL, DRONE_SAMPLES),
    ):
        off = PDX_BANK_BYTES + len(data)
        data += bytes([level]) * count
        struct.pack_into(">II", table, note * PDX_ENTRY_BYTES, off, count)

    return bytes(table) + bytes(data)


# ---- 自己検証 ------------------------------------------------------------


def parse_mdx(buf):
    """ファーム側の `parse_header()` と同じ手順でヘッダを読み返す。"""
    at = buf.find(b"\x0d\x0a\x1a")
    if at < 0:
        raise ValueError("no title terminator")
    title = buf[:at].decode("shift_jis")
    pat = at + 3
    pend = buf.index(b"\x00", pat)
    pdx = buf[pat:pend].decode("ascii")
    base = pend + 1

    first = struct.unpack_from(">H", buf, base + 2)[0]
    if first < 4 or (first & 1) != 0:
        raise ValueError("bad channel table")
    nch = (first - 2) // 2
    voice_off = struct.unpack_from(">H", buf, base)[0]
    if voice_off < first or base + voice_off > len(buf):
        raise ValueError("bad voice offset")
    offs = [
        base + struct.unpack_from(">H", buf, base + 2 + 2 * i)[0] for i in range(nch)
    ]
    return {
        "title": title,
        "pdx": pdx,
        "base": base,
        "nch": nch,
        "voice_top": base + voice_off,
        "mml": offs,
        "size": len(buf),
    }


def self_test():
    cases = []

    def check(name, ok, detail=""):
        cases.append((name, ok, detail))

    mdx = build_mdx()
    busy = build_mdx(busy=True)
    pdx = build_pdx()

    h = parse_mdx(mdx)
    check("title", h["title"] == TITLE, h["title"])
    check("pdx name", h["pdx"] == PDX_NAME, h["pdx"])
    check("channel count", h["nch"] == MDX_CH_COUNT, str(h["nch"]))
    check(
        "mml offsets in range",
        all(0 <= o < h["size"] for o in h["mml"]),
        str(h["mml"]),
    )
    check(
        "voice records are whole",
        (h["size"] - h["voice_top"]) % 27 == 0
        and h["size"] > h["voice_top"],
        "%d bytes" % (h["size"] - h["voice_top"]),
    )
    check("voice number 0", mdx[h["voice_top"]] == 0x00)

    # ループが自分の音符の先頭へ戻ること。0xF1 の 2 バイトを読んだ位置から戻る。
    for name, idx in (("fm", 0), ("pcm", MDX_FM_CH)):
        top = h["mml"][idx]
        end = mdx.index(bytes([OP_LOOP]), top)
        w = struct.unpack_from(">H", mdx, end + 1)[0]
        back = (-w) & 0xFFFF
        target = end + 3 - back
        # 戻り先は「先頭のコマンド列を飛ばした音符の位置」= 直前 2 バイトが音符
        check(
            "%s loop target is a note" % name,
            0x80 <= mdx[target] < 0xE0,
            "op %02x at %d" % (mdx[target], target),
        )
        check("%s loop word is not an end marker" % name, (w >> 8) != 0)

    # 使わないチャンネルは演奏終了
    for i in range(MDX_CH_COUNT):
        if i in (0, MDX_FM_CH):
            continue
        o = h["mml"][i]
        if mdx[o] != OP_LOOP or mdx[o + 1] != 0x00:
            check("ch%d ends" % i, False, "%02x %02x" % (mdx[o], mdx[o + 1]))
            break
    else:
        check("unused channels end", True)

    # PDX
    check(
        "pdx size",
        len(pdx) == PDX_BANK_BYTES + PCM_SAMPLES + DRONE_SAMPLES,
        str(len(pdx)),
    )
    for name, note, level, count in (
        ("hit", PCM_NOTE, PCM_LEVEL, PCM_SAMPLES),
        ("drone", DRONE_NOTE, DRONE_LEVEL, DRONE_SAMPLES),
    ):
        off, ln = struct.unpack_from(">II", pdx, note * PDX_ENTRY_BYTES)
        check("pdx %s entry" % name, ln == count, "%d %d" % (off, ln))
        check("pdx %s fits" % name, off + ln <= len(pdx))
        check("pdx %s payload" % name, set(pdx[off : off + ln]) == {level})
    check(
        "other pdx entries are empty",
        all(
            struct.unpack_from(">II", pdx, n * PDX_ENTRY_BYTES)[1] == 0
            for n in range(PDX_BANK_NOTES)
            if n not in (PCM_NOTE, DRONE_NOTE)
        ),
    )
    check(
        "drone is far below the analysis threshold",
        DRONE_LEVEL * 256 * 10387 // 65536 < 500,
        str(DRONE_LEVEL * 256 * 10387 // 65536),
    )

    # busy 条件。ドローンが増えるだけで、他は quiet と同じであること。
    hb = parse_mdx(busy)
    check("busy channel count", hb["nch"] == MDX_CH_COUNT, str(hb["nch"]))
    check("busy is longer", len(busy) > len(mdx))
    drone_top = hb["mml"][DRONE_MDX_CH]
    check("busy drone note", busy[drone_top : drone_top + 16].find(
        bytes([note_op(DRONE_NOTE), NOTE_CLOCKS - 1])) >= 0)
    check("busy drone pan is left", busy[drone_top : drone_top + 12].find(
        bytes([OP_PAN, 0x01])) >= 0)
    check(
        "quiet has no drone",
        mdx[h["mml"][DRONE_MDX_CH]] == OP_LOOP
        and mdx[h["mml"][DRONE_MDX_CH] + 1] == 0x00,
    )

    # 定位が左右で分かれていること（分離できないと測定にならない）
    fm_top = h["mml"][0]
    pcm_top = h["mml"][MDX_FM_CH]
    check("fm pan is right", mdx[fm_top : fm_top + 12].find(bytes([OP_PAN, 0x02])) >= 0)
    check("pcm pan is left", mdx[pcm_top : pcm_top + 12].find(bytes([OP_PAN, 0x01])) >= 0)

    bad = 0
    for name, ok, detail in cases:
        if ok:
            print("PASS  %s" % name)
        else:
            bad += 1
            print("FAIL  %s  %s" % (name, detail))
    print("%d cases, %d failed" % (len(cases), bad))
    return 1 if bad else 0


# ---- main ----------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    ap.add_argument("-o", "--out-dir", default=os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument("--self-test", action="store_true", help="生成物を検証して終わる")
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    for name, data in (
        ("SYNC.MDX", build_mdx()),
        ("SYNC2.MDX", build_mdx(busy=True)),
        ("SYNC.PDX", build_pdx()),
    ):
        path = os.path.join(args.out_dir, name)
        with open(path, "wb") as fp:
            fp.write(data)
        print("%s  %d bytes" % (path, len(data)))

    clock_us = 1024 * (256 - TEMPO) / 4.0
    print(
        "note every %d clocks = %.2f ms  (tempo %d, phiM 4MHz)"
        % (NOTE_CLOCKS, NOTE_CLOCKS * clock_us / 1000.0, TEMPO)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
