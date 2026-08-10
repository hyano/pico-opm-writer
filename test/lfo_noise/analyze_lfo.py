#!/usr/bin/env python3
"""
掃引した `.wav.zst` を解析して、`LFRQ` と実測 LFO 更新周期の対応を集計する。

[README.md](README.md) §1〜§3 に書いてある結論はすべてこのスクリプトの出力が根拠。
**README の数値を確かめたい / キャプチャし直したデータで検証し直したいときはこれを実行する。**

## やること

1. `tools/opm-lfo-period.py` を `--mode am` / `--mode pm` で走らせ、1 条件 1 行の TSV を
   `result/{wav-dir 名}_{mode}.tsv` に書く（既に在れば `--summary-only` で再利用できる）
2. その TSV から、README の各主張に対応する集計を出す

集計は主張ごとに 1 節。**どの数字がどの主張の根拠か**が出力を見て分かるようにしてある。

    [A] 掃引の完了度と confidence
    [B] 周期は LFRQ の上位ニブルだけで決まるか（下位ニブルを振ったときの散らばり）
    [C] AM 測定と PM 測定が独立に一致するか
    [D] NFRQ が決めるのは何か（速い側の頭打ち）
    [E] ymfm の想定モデルとの比較
    [F] 搬送波を変えた別データセットとの突き合わせ（--cross）
    [G] confidence が LFRQ に依存していないかの点検

## 使い方

```bash
./analyze_lfo.py                      # wav/ を解析して result/ に書き、集計を表示
./analyze_lfo.py --wav-dir ./wav_4a_01
./analyze_lfo.py --summary-only       # 解析はせず result/ の TSV から集計だけやり直す
./analyze_lfo.py --cross              # wav/ と wav_4a_01/ を突き合わせる（[F]）

# 個別条件の中身を見る: 更新イベントの間隔が更新周期の整数倍に揃っているかを直接確かめる
./analyze_lfo.py --mode am --gaps wav/am_nfrq_1f_lfrq_2{0,4,8,f}_kc_4a_mul_04.wav.zst
```

`--gaps` は集計ではなく**個別条件の生の中身**を見るためのもの。イベント間隔が推定周期の
整数倍に揃っていれば「更新周期は一定で、値が変わらなかった回がある」と読める。
`[B]` の外れ値がどこから来ているかはこれで確かめる。

解析には時間がかかる（`wav/` の 1024 条件で 7 分程度）。集計だけなら一瞬。

外部ライブラリは使わない（標準ライブラリのみ）。

SPDX-License-Identifier: MIT
"""

import argparse
import importlib.util
import math
import re
import subprocess
import sys
from collections import Counter
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ANALYZER = SCRIPT_DIR.parent.parent / "tools" / "opm-lfo-period.py"

MODES = ("am", "pm")
NFRQS = (0x00, 0x1F)

# ファイル名から条件を取り出す（README 付録 B.3）
NAME_RE = re.compile(r"_nfrq_([0-9a-f]{2})_lfrq_([0-9a-f]{2})"
                     r"_kc_([0-9a-f]{2})_mul_([0-9a-f]{2})")

# 「一致した」と見なす相対差
AGREE_TOL = 0.02
# 下限に張り付いていると見なす倍率
FLOOR_TOL = 1.5
# --gaps でイベント候補として拾う数。期待される更新回数の何倍まで見るか
# （少なすぎると間隔が水増しされ、多すぎると背景ノイズを拾う）
GAP_CANDIDATES = 1.5
# [G] で下限に張り付いていない帯だけを見るための上位ニブルの上限
NIBBLE_HI_MAX = 0x0D
# [G] で「下位ニブルに依存していない」と見なす confidence のずれ
LOWNIBBLE_TOL = 0.35


def model_period(lfrq):
    """ymfm が採っている想定モデルでの 1 段の長さ [samples]（README §4.1）。"""
    return 2 ** 22 / ((16 + (lfrq & 0x0F)) << (lfrq >> 4))


# ---- 解析の実行 ---------------------------------------------------------

def analyze(wav_dir, mode, out_tsv, quiet):
    """opm-lfo-period.py を掛けて TSV を作る。"""
    files = sorted(wav_dir.glob(f"{mode}_*.wav.zst"))
    if not files:
        print(f"! {wav_dir}: {mode}_*.wav.zst が無い", file=sys.stderr)
        return False
    argv = [sys.executable, str(ANALYZER), "--mode", mode] + [str(f) for f in files]
    print(f"* {mode}: {len(files)} 条件を解析 -> {out_tsv}", file=sys.stderr)
    out_tsv.parent.mkdir(parents=True, exist_ok=True)
    with out_tsv.open("w") as fp:
        proc = subprocess.run(argv, stdout=fp,
                              stderr=None if not quiet else subprocess.DEVNULL)
    if proc.returncode != 0:
        print(f"! {mode}: 解析器が終了コード {proc.returncode}"
              f"（読めない条件があった）", file=sys.stderr)
    return True


def load(tsv):
    """TSV を {(nfrq, lfrq): row} で読む。"""
    out = {}
    if not tsv.exists():
        return out
    lines = tsv.read_text().splitlines()
    for line in lines[1:]:
        f = line.split("\t")
        if len(f) < 7:
            continue
        m = NAME_RE.search(f[0])
        if not m:
            continue
        key = (int(m[1], 16), int(m[2], 16))
        out[key] = dict(period=float(f[4]), cycles=float(f[5]), conf=float(f[6]),
                        samples=int(f[3]), kc=int(m[3], 16), mul=int(m[4], 16))
    return out


# ---- 集計 ---------------------------------------------------------------

def section(title):
    print()
    print(title)
    print("-" * len(title))


def report_coverage(sets, label):
    section(f"[A] 掃引の完了度と confidence  ({label})")
    print("mode  条件数  confidence 最小 / 10%点 / 中央 / 最大")
    for mode in MODES:
        d = sets.get(mode) or {}
        if not d:
            print(f"{mode:4s}       0  （データ無し）")
            continue
        c = sorted(v["conf"] for v in d.values())
        n = len(c)
        print(f"{mode:4s}  {n:6d}  {c[0]:.3f} / {c[n//10]:.3f} / "
              f"{c[n//2]:.3f} / {c[-1]:.3f}")


def report_nibble(sets, label):
    section(f"[B] 周期は LFRQ の上位ニブルだけで決まるか  ({label})")
    print("下位ニブルを 0-f と振ったときの周期の散らばりを見る。")
    print("  想定モデルどおりなら 最大/最小 = 31/16 = 1.9375")
    print("  上位ニブルだけで決まるなら 最大/最小 = 1.0")
    print()
    print("mode nfrq  hi   中央値の周期    最大/最小   モデル(lo=0)   実測/モデル")
    for mode in MODES:
        for nfrq in NFRQS:
            d = sets.get(mode) or {}
            for hi in range(16):
                vals = [d[(nfrq, (hi << 4) | lo)]["period"]
                        for lo in range(16) if (nfrq, (hi << 4) | lo) in d]
                if len(vals) < 16:
                    continue
                vals.sort()
                med = vals[len(vals) // 2]
                mdl = model_period(hi << 4)
                print(f"{mode:4s}  {nfrq:02x}   {hi:x}  {med:12.2f}   "
                      f"{vals[-1]/vals[0]:8.4f}  {mdl:12.1f}   {med/mdl:8.4f}")
            print()


def report_agreement(sets, label):
    section(f"[C] AM 測定と PM 測定の一致  ({label})")
    print("同じ条件を振幅方向と位相方向から独立に測った結果の相対差。")
    print("  測定系ではなく音源側の性質を見ていることの裏付けになる。")
    print()
    am, pm = sets.get("am") or {}, sets.get("pm") or {}
    print("nfrq   件数   相対差 中央 /  90%点 /    最大")
    for nfrq in NFRQS:
        diffs = []
        for lfrq in range(256):
            a, p = am.get((nfrq, lfrq)), pm.get((nfrq, lfrq))
            if a and p and a["period"] > 0:
                diffs.append(abs(a["period"] - p["period"]) / a["period"])
        if not diffs:
            continue
        diffs.sort()
        n = len(diffs)
        print(f"  {nfrq:02x}  {n:5d}   {diffs[n//2]*100:9.4f}% / "
              f"{diffs[int(n*0.9)]*100:7.4f}% / {diffs[-1]*100:7.4f}%")


def report_floor(sets, label):
    section(f"[D] NFRQ が決めるのは何か  ({label})")
    print("速い側で周期が頭打ちになる値と、頭打ちが始まる LFRQ。")
    print()
    print("mode nfrq   下限 [samples]   頭打ちが始まる LFRQ")
    for mode in MODES:
        for nfrq in NFRQS:
            d = sets.get(mode) or {}
            vals = [(lfrq, v["period"]) for (nf, lfrq), v in d.items() if nf == nfrq]
            if not vals:
                continue
            floor = min(p for _, p in vals)
            first = None
            for lfrq, p in sorted(vals):
                if p <= floor * FLOOR_TOL:
                    first = lfrq
                    break
            where = f"{first:02x} 以上" if first is not None else "-"
            print(f"{mode:4s}  {nfrq:02x}   {floor:12.2f}       {where}")


def report_model(sets, label):
    section(f"[E] ymfm の想定モデルとの比較  ({label})")
    print("代表点での 実測 / モデル。1.000 ならモデルどおり。")
    print()
    print("lfrq  mode nfrq     実測 [samples]      モデル      実測/モデル  conf")
    for lfrq in (0x00, 0x08, 0x10, 0x20, 0x40, 0x60, 0x80, 0xA0,
                 0xC0, 0xE0, 0xF0, 0xFF):
        for mode in MODES:
            for nfrq in NFRQS:
                v = (sets.get(mode) or {}).get((nfrq, lfrq))
                if not v:
                    continue
                mdl = model_period(lfrq)
                print(f"  {lfrq:02x}  {mode:4s}  {nfrq:02x}  {v['period']:15.2f}  "
                      f"{mdl:11.2f}   {v['period']/mdl:10.4f}  {v['conf']:.3f}")
        print()


def report_cross(a_sets, b_sets, a_label, b_label):
    section(f"[F] 搬送波を変えた突き合わせ  ({a_label} vs {b_label})")
    print("同じ LFO 条件を別の搬送波でキャプチャしたデータと比べる。")
    print("  一致すれば、測った周期が搬送波由来のアーティファクトでないことの裏付けになる。")
    print("  不一致は「その搬送波では測れない条件」を示す（README 付録 B.4）。")
    print()
    for mode in MODES:
        a, b = a_sets.get(mode) or {}, b_sets.get(mode) or {}
        rows = [(nf, lf, a[(nf, lf)], b[(nf, lf)])
                for nf in NFRQS for lf in range(256)
                if (nf, lf) in a and (nf, lf) in b]
        if not rows:
            continue
        agree = [abs(r[2]["period"] - r[3]["period"]) / r[2]["period"] < AGREE_TOL
                 for r in rows]
        ok = [r for r, a in zip(rows, agree) if a]
        ng = [r for r, a in zip(rows, agree) if not a]
        print(f"{mode}: 一致 {len(ok)}/{len(rows)}  不一致 {len(ng)}")
        if ng:
            per = sorted(r[2]["period"] for r in ng)
            print(f"    不一致条件での {a_label} 側の周期: "
                  f"{per[0]:.1f} 〜 {per[-1]:.1f} samples")
            print(f"    うち周期が {b_label} の搬送波周期 (127 samples) 未満: "
                  f"{sum(1 for p in per if p < 127)}/{len(ng)}")
            print(f"    不一致条件の {b_label} 側 confidence 中央: "
                  f"{sorted(r[3]['conf'] for r in ng)[len(ng)//2]:.3f}")
            for r in ng[:5]:
                print(f"      nfrq={r[0]:02x} lfrq={r[1]:02x}  "
                      f"{a_label}={r[2]['period']:9.2f} (conf {r[2]['conf']:.2f})  "
                      f"{b_label}={r[3]['period']:9.2f} (conf {r[3]['conf']:.2f})")
        print()


def report_lownibble_conf(sets, label):
    section(f"[G] confidence が LFRQ に依存していないかの点検  ({label})")
    print("周期は下位ニブルで変わらない（[B]）のだから、confidence も変わらないはず。")
    print("  ただしキャプチャ長は想定モデルから決めているので、下位ニブルが大きいほど")
    print("  短くなり、観測できるサイクル数は 32 -> 16.5 まで減る。それでも confidence が")
    print("  系統的に落ちるなら、解析器が信号ではなく**キャプチャ長**を拾っている。")
    print(f"  下限に張り付く帯を除くため、上位ニブル 0-{NIBBLE_HI_MAX:x} だけを集計する。")
    print()
    print("lo    am nfrq=00   am nfrq=1f   pm nfrq=00   pm nfrq=1f   （confidence 中央値）")
    ends = {}
    for lo in range(16):
        cells = []
        for mode in MODES:
            for nfrq in NFRQS:
                d = sets.get(mode) or {}
                vals = sorted(d[(nfrq, (hi << 4) | lo)]["conf"]
                              for hi in range(NIBBLE_HI_MAX + 1)
                              if (nfrq, (hi << 4) | lo) in d)
                if vals:
                    med = vals[len(vals) // 2]
                    cells.append(f"{med:.3f}")
                    ends.setdefault((mode, nfrq), {})[lo] = med
                else:
                    cells.append("  -  ")
        print(f" {lo:x}       " + "        ".join(cells))

    print()
    worst = 0.0
    for (mode, nfrq), by_lo in sorted(ends.items()):
        if 0 in by_lo and 15 in by_lo and by_lo[0] > 0:
            ratio = by_lo[15] / by_lo[0]
            worst = max(worst, abs(1.0 - ratio))
            print(f"  {mode} nfrq={nfrq:02x}: lo=f / lo=0 = {ratio:.3f}")
    verdict = "OK（依存なし）" if worst <= LOWNIBBLE_TOL else "NG（依存が残っている）"
    print(f"  最大のずれ {worst*100:.0f}%  -> {verdict}"
          f"（許容 {LOWNIBBLE_TOL*100:.0f}%）")


def report_gaps(paths, mode):
    """個別条件の更新イベント間隔を推定周期で正規化して並べる。"""
    spec = importlib.util.spec_from_file_location("opm_lfo_period", ANALYZER)
    olp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(olp)

    section(f"更新イベントの間隔（--mode {mode}）")
    print("推定周期を 1 として、隣り合う更新イベントの間隔を数える。")
    print("  1.0 に揃う  -> 毎回の更新が見えている")
    print("  2.0, 3.0 が混じる -> その回は値が変わらず更新が見えなかった")
    print("  整数倍でない値が多い -> 周期の推定自体を疑う")
    print()
    for path in paths:
        x, _ = olp.load_left(path)
        a = olp.Analyzer(x, mode)
        period, conf = a.run(lambda msg: None)

        s = max(1, 1 << int(math.log2(max(1.0, period / 64))))
        d = a.feature(s, 1 << 16)
        if not d:
            print(f"* {Path(path).name}: 特徴量が取れない")
            continue
        # 期待される更新回数から閾値を決める。2 つの更新が 1 周期より近づくことは
        # 無いので、ピークの間隔は周期の半分以上を要求する。
        slots = max(4, int(len(d) * s / period))
        rank = min(len(d) - 1, int(slots * GAP_CANDIDATES))
        thr = sorted(d, reverse=True)[rank]
        apart = max(2, int(period / (2 * s)))
        peaks = []
        for i, v in enumerate(d):
            if v >= thr and (not peaks or i - peaks[-1] >= apart):
                peaks.append(i)
        gaps = [(b - c) * s / period for c, b in zip(peaks, peaks[1:])]
        gaps = [g for g in gaps if g > 0.25]
        print(f"* {Path(path).name}")
        print(f"    周期 {period:.2f} samples / confidence {conf:.3f} / "
              f"イベント {len(peaks)} 個")
        if not gaps:
            print("    間隔を測れるイベントが足りない")
            continue
        hist = Counter(round(g * 4) / 4 for g in gaps)
        integral = sum(n for g, n in hist.items() if abs(g - round(g)) < 0.13)
        print("    間隔のヒストグラム: "
              + ", ".join(f"{g:g}x{n}" for g, n in sorted(hist.items())[:10]))
        print(f"    整数倍に乗っている割合: {integral}/{len(gaps)}"
              f" ({integral/len(gaps)*100:.0f}%)")


# ---- エントリポイント ---------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(
        description="掃引した .wav.zst を解析して LFRQ と実測周期の対応を集計する")
    parser.add_argument("--wav-dir", type=Path, default=SCRIPT_DIR / "wav",
                        help="解析するキャプチャのディレクトリ（既定 ./wav）")
    parser.add_argument("--result-dir", type=Path, default=SCRIPT_DIR / "result",
                        help="TSV の置き場（既定 ./result）")
    parser.add_argument("--summary-only", action="store_true",
                        help="解析はせず、既存の TSV から集計だけやり直す")
    parser.add_argument("--cross", action="store_true",
                        help="wav/ と wav_4a_01/ を突き合わせる（[F]）")
    parser.add_argument("--gaps", type=Path, nargs="+", metavar="WAV",
                        help="個別条件の更新イベント間隔を見る（--mode が要る）")
    parser.add_argument("--mode", choices=MODES,
                        help="--gaps で使う解析モード")
    parser.add_argument("--quiet", action="store_true",
                        help="解析器の警告を捨てる")
    args = parser.parse_args(argv)

    if not ANALYZER.exists():
        print(f"! 解析器が見つからない: {ANALYZER}", file=sys.stderr)
        return 1

    if args.gaps:
        if not args.mode:
            parser.error("--gaps には --mode が要る")
        report_gaps(args.gaps, args.mode)
        return 0

    def gather(wav_dir):
        label = wav_dir.name
        sets = {}
        for mode in MODES:
            tsv = args.result_dir / f"{label}_{mode}.tsv"
            if not args.summary_only:
                if not analyze(wav_dir, mode, tsv, args.quiet):
                    continue
            sets[mode] = load(tsv)
            if not sets[mode]:
                print(f"! {tsv}: 集計できる行が無い", file=sys.stderr)
        return label, sets

    if args.cross:
        a_label, a_sets = gather(SCRIPT_DIR / "wav")
        b_label, b_sets = gather(SCRIPT_DIR / "wav_4a_01")
        report_cross(a_sets, b_sets, a_label, b_label)
        return 0

    label, sets = gather(args.wav_dir)
    if not any(sets.values()):
        print("! 集計できるデータが無い", file=sys.stderr)
        return 1
    report_coverage(sets, label)
    report_nibble(sets, label)
    report_agreement(sets, label)
    report_floor(sets, label)
    report_model(sets, label)
    report_lownibble_conf(sets, label)
    return 0


if __name__ == "__main__":
    sys.exit(main())
