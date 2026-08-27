#!/usr/bin/env python3
"""
FM と ADPCM の立ち上がりのずれをキャプチャから測る。

[README.md](README.md) の結論はすべてこのスクリプトの出力が根拠。

## 何を測るか

[gen_testdata.py](gen_testdata.py) が作る `SYNC.MDX` は、FM を右・ADPCM を左に振り分けて
**同じ tick で同時にキーオン**し続ける。したがってキャプチャの中では

    ずれ = (R の立ち上がりフレーム) - (L の立ち上がりフレーム)

が 1 音ぶんのずれになる。**正なら ADPCM のほうが早い。**

## 立ち上がりの決め方

無音が `--guard` フレーム以上続いたあと、最初に `--threshold` を超えたフレームを
立ち上がりとする。無音区間は厳密に 0 になる（FM は反対側のスロットへ何も加算せず、
ADPCM のミックスリングも片側は 0 のまま）ので、閾値は雑音を跨ぐためではなく
**アタックのごく初期の 1-2 フレームを無視するため**にある。

閾値のぶんだけ検出は実際の立ち上がりより遅れるが、その遅れは L / R それぞれで一定なので
**分布の幅と、条件間での平均の差**には効かない。ここで読むのはその 2 つ。

## 使い方

```bash
./analyze_sync.py wav/before.wav.zst              # 要約を出す
./analyze_sync.py wav/*.wav.zst                   # 複数条件を並べて比べる
./analyze_sync.py wav/before.wav.zst --tsv        # 1 音 1 行の TSV
./analyze_sync.py --self-test                     # 検出器の自己検証（実機不要）
```

SPDX-License-Identifier: MIT
"""

import argparse
import array
import io
import math
import os
import sys
import wave

try:
    from compression.zstd import ZstdFile  # Python 3.14+
except ImportError:  # pragma: no cover - 3.13 以前
    ZstdFile = None

ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"

# 既定値。SYNC.MDX は 6 clock = 24.58ms ごと（φM 4MHz、@t 240）に鳴るので、
# 1 音の間隔はおよそ 1536 フレーム。
DEFAULT_THRESHOLD = 500
DEFAULT_GUARD = 64


class InputError(Exception):
    pass


# ---- 入力 ----------------------------------------------------------------


def read_capture(path):
    """`.wav` / `.wav.zst` を読んで (rate, L, R) を返す。"""
    with open(path, "rb") as fp:
        blob = fp.read()

    if blob[: len(ZSTD_MAGIC)] == ZSTD_MAGIC:
        if ZstdFile is None:
            raise InputError(
                "zstd 圧縮の入力には Python 3.14 以降が必要"
                "（`zstd -dc FILE > FILE.wav` で展開してから渡す）"
            )
        blob = ZstdFile(io.BytesIO(blob), "rb").read()

    with wave.open(io.BytesIO(blob)) as w:
        if w.getnchannels() != 2 or w.getsampwidth() != 2:
            raise InputError("signed 16bit ステレオの WAV でなければならない")
        rate = w.getframerate()
        pcm = array.array("h")
        pcm.frombytes(w.readframes(w.getnframes()))

    if sys.byteorder == "big":
        pcm.byteswap()
    return rate, pcm[0::2], pcm[1::2]


# ---- 立ち上がりの検出 ----------------------------------------------------


def find_onsets(ch, threshold, guard):
    """
    無音が `guard` フレーム続いたあと最初に `threshold` を超えたフレームの番号を返す。

    「無音が続いたあと」を条件にしているのは、1 音の途中で波形が閾値を上下しても
    立ち上がりを二重に数えないため。
    """
    out = []
    quiet = guard  # 先頭は無音の続きとみなす
    for i, v in enumerate(ch):
        if v > -threshold and v < threshold:
            quiet += 1
            continue
        if quiet >= guard:
            out.append(i)
        quiet = 0
    return out


def pair_onsets(l_on, r_on, window):
    """
    L と R の立ち上がりを 1 対 1 に対応づけて (l, r, r - l) の列を返す。

    対応づけは「L の各立ち上がりに対し、`window` フレーム以内で最も近い R」。
    どちらかが取りこぼされた音（対にならなかったもの）は捨てる。
    """
    pairs = []
    j = 0
    used = -1
    for l in l_on:
        while j < len(r_on) and r_on[j] < l - window:
            j += 1
        best = None
        k = j
        while k < len(r_on) and r_on[k] <= l + window:
            if best is None or abs(r_on[k] - l) < abs(best - l):
                best = r_on[k]
            k += 1
        if best is None or best <= used:
            continue
        used = best
        pairs.append((l, best, best - l))
    return pairs


# ---- 集計 ----------------------------------------------------------------


def summarize(pairs, rate):
    d = [p[2] for p in pairs]
    n = len(d)
    if n == 0:
        return None
    mean = sum(d) / n
    var = sum((x - mean) ** 2 for x in d) / n
    s = sorted(d)
    return {
        "n": n,
        "min": s[0],
        "max": s[-1],
        "span": s[-1] - s[0],
        "mean": mean,
        "sd": math.sqrt(var),
        "median": s[n // 2],
        "mean_us": mean / rate * 1e6,
        "span_us": (s[-1] - s[0]) / rate * 1e6,
        "hist": d,
    }


def histogram(values, width=48, bins=16):
    lo, hi = min(values), max(values)
    if hi == lo:
        hi = lo + 1
    step = (hi - lo) / bins
    counts = [0] * bins
    for v in values:
        k = int((v - lo) / step)
        counts[min(k, bins - 1)] += 1
    top = max(counts) or 1
    lines = []
    for k, c in enumerate(counts):
        a = lo + k * step
        b = a + step
        bar = "#" * int(round(c * width / top))
        lines.append("  %7.1f .. %7.1f | %-*s %d" % (a, b, width, bar, c))
    return lines


def report(path, rate, pairs, show_hist):
    st = summarize(pairs, rate)
    print("%s" % path)
    if st is None:
        print("  立ち上がりの対が見つからない")
        return
    print(
        "  対 %d 個  ずれ min %d  max %d  幅 %d  平均 %.2f  中央 %d  標準偏差 %.2f  "
        "フレーム" % (st["n"], st["min"], st["max"], st["span"], st["mean"],
                      st["median"], st["sd"])
    )
    print(
        "  平均 %.1f us  幅 %.1f us  (%d Hz。正 = ADPCM のほうが早い)"
        % (st["mean_us"], st["span_us"], rate)
    )
    if show_hist:
        for line in histogram(st["hist"]):
            print(line)


# ---- 自己検証 ------------------------------------------------------------


def synth(length, onsets, level, attack=0):
    """`onsets` のフレームから `level` まで `attack` フレームで立ち上がる矩形を作る。"""
    ch = array.array("h", [0]) * length
    for at in onsets:
        for i in range(at, min(length, at + 400)):
            k = i - at
            v = level if k >= attack else int(level * (k + 1) / (attack + 1))
            ch[i] = v
    return ch


def self_test():
    cases = []

    def check(name, ok, detail=""):
        cases.append((name, ok, detail))

    # 1. 単純な矩形。立ち上がりがそのまま出る。
    ch = synth(4000, [100, 1600, 3100], 20000)
    on = find_onsets(ch, DEFAULT_THRESHOLD, DEFAULT_GUARD)
    check("矩形の立ち上がり", on == [100, 1600, 3100], str(on))

    # 2. 音の途中で閾値を割っても二重に数えない（ガードより短い無音は無視）。
    ch = synth(3000, [100], 20000)
    for i in range(200, 210):
        ch[i] = 0
    on = find_onsets(ch, DEFAULT_THRESHOLD, DEFAULT_GUARD)
    check("途中の短い無音を跨がない", on == [100], str(on))

    # 3. 閾値より小さい信号は拾わない。
    ch = synth(2000, [100], DEFAULT_THRESHOLD - 1)
    check("閾値未満は拾わない", find_onsets(ch, DEFAULT_THRESHOLD, DEFAULT_GUARD) == [])

    # 4. 負側だけの信号も拾う。
    ch = synth(2000, [100], -20000)
    check("負側も拾う", find_onsets(ch, DEFAULT_THRESHOLD, DEFAULT_GUARD) == [100])

    # 5. 傾斜のあるアタックは閾値を跨いだ位置で検出される（一定のバイアス）。
    ch = synth(2000, [100], 20000, attack=8)
    on = find_onsets(ch, 10000, DEFAULT_GUARD)
    check("傾斜は閾値を跨いだ位置", on == [104], str(on))

    # 6. 対応づけ。既知のずれを入れて取り出せること。
    lo = [100, 1600, 3100, 4600]
    skew = [40, 10, 93, 0]
    ro = [l + s for l, s in zip(lo, skew)]
    pairs = pair_onsets(lo, ro, 768)
    check("対応づけ", [p[2] for p in pairs] == skew, str([p[2] for p in pairs]))

    # 7. 片側が欠けた音は捨てる。
    pairs = pair_onsets([100, 1600, 3100], [140, 3193], 768)
    check("片側欠けを捨てる", [p[2] for p in pairs] == [40, 93], str(pairs))

    # 8. window の外にある立ち上がりは対にしない。
    pairs = pair_onsets([100], [100 + 900], 768)
    check("window の外は対にしない", pairs == [], str(pairs))

    # 9. 同じ R を 2 つの L に割り当てない。
    pairs = pair_onsets([100, 200], [150], 768)
    check("R の重複割り当てが無い", len(pairs) == 1, str(pairs))

    # 10. 端から端まで通した統合テスト。既知のずれを復元できること。
    n = 62500
    lo = list(range(500, n - 2000, 1536))
    skew = [(i * 37) % 94 for i in range(len(lo))]
    L = synth(n, lo, 24576)
    R = synth(n, [l + s for l, s in zip(lo, skew)], 8000, attack=1)
    pairs = pair_onsets(
        find_onsets(L, DEFAULT_THRESHOLD, DEFAULT_GUARD),
        find_onsets(R, DEFAULT_THRESHOLD, DEFAULT_GUARD),
        768,
    )
    got = [p[2] for p in pairs]
    check("統合: 対の数", len(got) == len(lo), "%d / %d" % (len(got), len(lo)))
    check("統合: ずれの復元", got == skew, str(got[:8]))
    st = summarize(pairs, 62500)
    check("統合: 幅", st["span"] == max(skew) - min(skew), str(st["span"]))
    check(
        "統合: 平均 us",
        abs(st["mean_us"] - sum(skew) / len(skew) / 62500 * 1e6) < 1e-6,
        str(st["mean_us"]),
    )

    # 11. 対が無いとき summarize は None
    check("空の集計", summarize([], 62500) is None)

    # 12. ヒストグラムは全件を数える
    lines = histogram([0, 1, 2, 3, 4, 5, 6, 7], bins=4)
    total = sum(int(l.rsplit(" ", 1)[1]) for l in lines)
    check("ヒストグラムの総数", total == 8, str(total))

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
    ap.add_argument("wav", nargs="*", help="キャプチャ（.wav / .wav.zst）")
    ap.add_argument("--threshold", type=int, default=DEFAULT_THRESHOLD,
                    help="立ち上がりと見なす絶対値（既定 %d）" % DEFAULT_THRESHOLD)
    ap.add_argument("--guard", type=int, default=DEFAULT_GUARD,
                    help="立ち上がりの前に要る無音フレーム数（既定 %d）" % DEFAULT_GUARD)
    ap.add_argument("--window", type=int, default=768,
                    help="L と R を対にする最大の隔たり（フレーム。既定 768）")
    ap.add_argument("--tsv", action="store_true", help="1 音 1 行の TSV を出す")
    ap.add_argument("--no-hist", action="store_true", help="ヒストグラムを出さない")
    ap.add_argument("--self-test", action="store_true", help="検出器を検証して終わる")
    args = ap.parse_args()

    if args.self_test:
        return self_test()
    if not args.wav:
        ap.error("キャプチャを 1 つ以上指定するか --self-test を使う")

    if args.tsv:
        print("file\tindex\tl_frame\tr_frame\tskew_frames\tskew_us")

    rc = 0
    for path in args.wav:
        try:
            rate, L, R = read_capture(path)
        except (InputError, OSError, wave.Error) as e:
            print("%s: %s" % (path, e), file=sys.stderr)
            rc = 1
            continue

        pairs = pair_onsets(
            find_onsets(L, args.threshold, args.guard),
            find_onsets(R, args.threshold, args.guard),
            args.window,
        )
        if args.tsv:
            name = os.path.basename(path)
            for i, (l, r, d) in enumerate(pairs):
                print("%s\t%d\t%d\t%d\t%d\t%.2f"
                      % (name, i, l, r, d, d / rate * 1e6))
        else:
            report(path, rate, pairs, not args.no_hist)
    return rc


if __name__ == "__main__":
    sys.exit(main())
