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
    [D] NFRQ ごとに「値が変わる間隔」が最短でいくつになるか
    [E] ymfm の想定モデルとの比較
    [F] 搬送波を変えた別データセットとの突き合わせ（--cross）
    [G] confidence が LFRQ に依存していないかの点検
    [J] NFRQ と値の供給周期（--supply）

## 使い方

```bash
./analyze_lfo.py                      # wav/ を解析して result/ に書き、集計を表示
./analyze_lfo.py --wav-dir ./wav_4a_01
./analyze_lfo.py --summary-only       # 解析はせず result/ の TSV から集計だけやり直す
./analyze_lfo.py --cross              # wav/ と wav_4a_01/ を突き合わせる（[F]）
./analyze_lfo.py --supply --wav-dir ./wav_nfrq_5a09    # [J]
./analyze_lfo.py --self-test          # --runs を人工信号で検証する（実機不要）

# 個別条件の中身を見る: 更新イベントの間隔が更新周期の整数倍に揃っているかを直接確かめる
./analyze_lfo.py --mode am --gaps wav/am_nfrq_1f_lfrq_2{0,4,8,f}_kc_4a_mul_04.wav.zst

# 段ごとの LFO 値を取り出し、同じ語を続けて引いた回数から値の供給周期を出す
./analyze_lfo.py --mode am --period 8 --runs wav_nfrq_5a09/am_nfrq_00_lfrq_ff_*.wav.zst
```

`--gaps` は集計ではなく**個別条件の生の中身**を見るためのもの。イベント間隔が推定周期の
整数倍に揃っていれば「更新周期は一定で、値が変わらなかった回がある」と読める。
`[B]` の外れ値がどこから来ているかはこれで確かめる。

`--runs` は更新の**タイミング**ではなく**値そのもの**を見る。`--period` で更新周期を
渡せるのが要で、値が 1 回おきにしか変わらない条件（`NFRQ=0x00` の `f` 帯）でも
「LFO が本当は何サンプルごとにラッチしているか」を仮定して問い直せる。

解析には時間がかかる（`wav/` の 1024 条件で 7 分程度）。集計だけなら一瞬。

外部ライブラリは使わない（標準ライブラリのみ）。

SPDX-License-Identifier: MIT
"""

import argparse
import bisect
import importlib.util
import math
import re
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ANALYZER = SCRIPT_DIR.parent.parent / "tools" / "opm-lfo-period.py"
TESTGEN = SCRIPT_DIR.parent.parent / "tools" / "opm-lfo-period-testgen.py"

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
# --runs で段の頭を捨てる幅 [samples]（特徴量のカーネルが 3 サンプル幅なので 2）
BLOCK_GUARD = 2
# --runs で独立ペアの分布のどこを基準点に取るか（下側の分位）
FAR_Q = 0.10


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
    section(f"[D] NFRQ ごとに値が変わる間隔の最小値  ({label})")
    print("値が変わる間隔が最短でいくつになるかと、そこに達する LFRQ。")
    print("  これは LFO の更新周期の頭打ちではなく、ノイズ語の供給周期との合成の結果。")
    print("  供給周期そのものは [J] (--supply) で測る。")
    print()
    print("mode nfrq   最小 [samples]   そこに達する LFRQ")
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
    olp = load_analyzer()
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


# ---- 段ごとの LFO 値と連の長さ -----------------------------------------

def load_analyzer():
    """tools/opm-lfo-period.py をモジュールとして読む。"""
    spec = importlib.util.spec_from_file_location("opm_lfo_period", ANALYZER)
    olp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(olp)
    return olp


def block_values(a, mode, period, off, guard=BLOCK_GUARD):
    """ラッチ格子 [off + iP + guard, off + (i+1)P) ごとの LFO 値を返す。

    `build_sums` が作った累積和からブロックの値を復元するだけ。AM は
    `0.5 log Σenv²`（= 対数振幅）、PM は最小二乗の cos(ω)（= 搬送波の周波数）で、
    どちらもその段の LFO 値の単調関数になる。`level_feature` はこの 1 階差分しか
    返さないので、値そのものはここで作る。

    `guard` は段の頭を捨てる幅。包絡も cos(ω) も 3 サンプルのカーネル
    （`x[j-1], x[j], x[j+1]`）で作るので、段の境界をまたぐ 2 サンプルは前後の段の
    値が混ざる。P=8 では 8 サンプル中 2 なので効きが大きく、捨てないと隣り合う段の
    値が引き寄せ合って「変わっていない」側に倒れる。
    """
    nb = (a.m - off) // period
    if nb < 4 or period - guard < 2:
        return []
    if mode == "am":
        acc = a.sums[0]
        out = []
        for i in range(nb):
            v = acc[off + (i + 1) * period] - acc[off + i * period + guard]
            out.append(0.5 * math.log(v) if v > 0.0 else 0.0)
        return out
    p, q = a.sums
    out = []
    for i in range(nb):
        lo, hi = off + i * period + guard, off + (i + 1) * period
        den = q[hi] - q[lo]
        out.append((p[hi] - p[lo]) / (2.0 * den) if den > 0.0 else 0.0)
    return out


def changed_fraction(v):
    """「値が変わった段の割合 c」を、隣接ペアと遠いペアの分布の比から出す。

    連長が 1 か 2 しか出ない帯（`P_lfo <= P_n <= 2 P_lfo`）では、隣り合う段の
    `|Δ値|` は 2 つの分布の混合になる。

        F1(t) = (1-c) · F0(t) + c · Fk(t)

    `F0` は「値が変わらなかった段」の分布で、正しく整列していれば測定誤差だけの
    幅しか持たない。`Fk` は 3 段以上離れたペア（必ず別の語）の分布で、**同じ
    データから実測できる**。基準点 `t` を `Fk` の下側 10% 点に取れば `F0(t) ≈ 1` なので

        c = (1 - F1(t)) / (1 - Fk(t))

    となり、測定誤差の大きさにも LFO 値の分布の形にも依存しない。閾値を手で
    与えないので、結論が閾値の選び方に左右されない。
    """
    n = len(v)
    if n < 256:
        return None, None
    d1 = sorted(abs(b - c) for c, b in zip(v, v[1:]))
    far = sorted(abs(v[i + k] - v[i])
                 for k in range(5, 21) for i in range(0, n - k, 7))
    if len(far) < 256:
        return None, None
    t = far[int(len(far) * FAR_Q)]
    if t <= 0.0:
        return None, None
    f1 = bisect.bisect_left(d1, t) / len(d1)
    c = (1.0 - f1) / (1.0 - FAR_Q)
    return min(1.0, max(1e-6, c)), t


def step_runs(olp, path, mode, period=None):
    """段ごとの LFO 値を取り出し、同じ値が続く連の長さを数える。

    返すのは (period, conf, 連長のリスト, 変化した割合 c, 格子の位相)。
    **`period / c` が値の供給側の周期**（ノイズ発生器が新しい語を出す周期）になる。
    `c` は閾値を通さないモーメント法で出し、連長のヒストグラムは 3σ 判定で作る。
    """
    x, _ = olp.load_left(path)
    a = olp.Analyzer(x, mode)
    conf = None
    if period is None:
        period, conf = a.run(lambda msg: None)
    p = int(round(period))
    if p < 2 or abs(period - p) > 0.05 * p:
        raise ValueError(f"格子にできる整数周期でない: {period:.3f}")

    # 格子の位相は総当たりで決める（候補は p 通りしかない）。ずれていると 1 つの
    # ブロックが 2 つの語にまたがり、変わっていない段まで変わって見えるので、
    # **c が最小になる位相**が正しい整列。
    best = None
    for off in range(p):
        v = block_values(a, mode, p, off)
        c, t = changed_fraction(v)
        if c is None:
            continue
        if best is None or c < best[0]:
            best = (c, t, off, v)
    if best is None:
        raise ValueError("段が足りない")
    frac, thr, off, v = best

    at = [i for i, (c, b) in enumerate(zip(v, v[1:])) if abs(b - c) >= thr]
    runs = [b - c for c, b in zip(at, at[1:])]
    return p, conf, runs, frac, off


def report_runs(paths, mode, period=None):
    """個別条件の連の長さを並べ、値の供給周期を出す。"""
    olp = load_analyzer()
    section(f"段ごとの LFO 値と連の長さ（--mode {mode}）")
    print("ラッチ格子ごとの LFO 値を取り出し、同じ値が続く連の長さを数える。")
    print("  供給周期 = 更新周期 / 変化した段の割合。割合は閾値を通さずに出す")
    print("  連長が全部 1 -> 毎回新しい語を引いている")
    print("  連長に奇数が無い -> 供給が更新のちょうど 2 倍遅く、位相が噛み合っている")
    print()
    for path in paths:
        name = Path(path).name
        try:
            p, conf, runs, frac, off = step_runs(olp, path, mode, period)
        except Exception as e:                        # noqa: BLE001
            print(f"* {name}: {e}")
            continue
        hist = Counter(runs)
        cs = f" / confidence {conf:.3f}" if conf is not None else ""
        odd = sum(n for g, n in hist.items() if g % 2)
        print(f"* {name}")
        print(f"    更新周期 {p} samples{cs} / 位相 {off} / 連 {len(runs)} 本")
        # ヒストグラムの閾値は独立ペアの下側 10% 点なので、真の変化の 1 割は
        # 取りこぼす。連の本数ではなく**長さの分布の形**を読むためのもの。
        print("    連長のヒストグラム: "
              + ", ".join(f"{g}x{n}" for g, n in sorted(hist.items())[:8])
              + f" / 奇数 {odd / len(runs) * 100:.1f}%")
        print(f"    変化した段 {frac * 100:.1f}% -> 供給周期 {p / frac:.2f} samples")


def lfo_period(lfrq):
    """§2.1 の規則で決まる LFO の更新周期 [samples]。上位ニブルだけで決まる。"""
    return 2 ** (18 - (lfrq >> 4))


def report_supply(wav_dir):
    """[J] NFRQ ごとの「値の供給周期」を全条件で集計する。

    更新周期は推定させず **§2.1 の規則値を渡す**。`NFRQ=0x00` の `f` 帯では
    値が 1 回おきにしか変わらないため周期推定が 16 を返してしまい、そのままでは
    「LFO が 8 でラッチしているか」を問えないため。
    """
    olp = load_analyzer()
    files = sorted(wav_dir.glob("*.wav.zst"))
    got = {}
    for path in files:
        m = NAME_RE.search(path.name)
        if not m:
            continue
        nfrq, lfrq = int(m[1], 16), int(m[2], 16)
        mode = path.name.split("_")[0]
        if mode not in MODES:
            continue
        try:
            p, _, runs, frac, _ = step_runs(olp, path, mode, lfo_period(lfrq))
        except Exception as e:                    # noqa: BLE001
            print(f"! {path.name}: {e}", file=sys.stderr)
            continue
        odd = sum(1 for r in runs if r % 2) / len(runs) if runs else float("nan")
        got[(mode, lfrq, nfrq)] = (p / frac, odd)

    section(f"[J] NFRQ と値の供給周期  ({wav_dir.name})")
    print("段ごとの LFO 値を取り出し、同じ語を続けて引いた回数から供給周期を出す。")
    print("  予測は P_n = (32 - NFRQ) / 2 [samples]")
    print("  奇数連% は「連の長さが奇数だった割合」。ちょうど 2 倍でロックしていれば 0")
    print("  変化した段の割合が 1 に近い側（NFRQ が大きい側）は原理的に飽和する")
    lfrqs = sorted({k[1] for k in got})
    for mode in MODES:
        for lfrq in lfrqs:
            rows = [(nf, got[(mode, lfrq, nf)])
                    for nf in range(32) if (mode, lfrq, nf) in got]
            if not rows:
                continue
            p = lfo_period(lfrq)
            print(f"\n  [{mode}] LFRQ={lfrq:02x}  更新周期 {p} samples")
            print("  nfrq   予測 P_n   実測 供給周期   実測/予測   奇数連%")
            for nf, (sup, odd) in rows:
                want = (32 - nf) / 2
                print(f"    {nf:02x}   {want:8.1f}   {sup:12.2f}   "
                      f"{sup/want:9.3f}   {odd*100:6.1f}")


# ---- 自己検証 -----------------------------------------------------------

# 供給周期の推定に許す相対誤差
RUNS_TOL = 0.03
# 飽和側（変化した段の割合 c が 1 に近い）で許す相対誤差
RUNS_SAT_TOL = 0.08


def self_test():
    """既知の供給周期を入れた人工信号で `--runs` を検証する（実機不要）。

    `tools/opm-lfo-period-testgen.py` の `synth(..., noise_period=)` が
    「段は period ごとに来るが、値の供給は noise_period ごと」という実機の構造を
    そのまま作れるので、連の平均長 × period がその noise_period に戻るかを見る。

    実機データで使う自己校正の 2 点（供給が 2 倍ちょうど遅いと連長は全部 2 /
    供給の方が速いと全部 1）もここで確かめる。
    """
    olp = load_analyzer()
    spec = importlib.util.spec_from_file_location("testgen", TESTGEN)
    tg = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(tg)

    period, length = 8, 1 << 19                   # 8 サンプル段 = 実機の f 帯と同じ

    # (供給周期, 許容, 適用するモード, 説明)。tol=None は「値は当てにせず、
    # 8 を下回らないことだけ見る」= 飽和側の性質だけを確かめるケース。
    cases = (
        (16.0, RUNS_TOL, MODES, "ちょうど 2 倍でロック（NFRQ=0x00 相当）"),
        (15.5, RUNS_TOL, MODES, "NFRQ=0x01 相当"),
        (12.0, RUNS_TOL, MODES, "NFRQ=0x08 相当"),
        (8.5, RUNS_SAT_TOL, ("pm",), "NFRQ=0x0f 相当"),
        (8.0, RUNS_SAT_TOL, ("pm",), "供給の方が速い（NFRQ>=0x10 相当）"),
        (8.5, None, ("am",), "AM は c が 1 に近い側で飽和する"),
        (8.0, None, ("am",), "AM は c が 1 に近い側で飽和する"),
    )

    results = []
    with tempfile.TemporaryDirectory() as tmp:
        for pn, tol, modes, note in cases:
            for mode in modes:
                path = Path(tmp) / f"{mode}_pn{pn:g}.wav"
                if not path.exists():
                    tg.write_wav(path, tg.synth(mode, period, length,
                                                tg.CARRIERS[0], 4200,
                                                noise_period=pn, iid=True))
                name = f"{mode} 供給周期 {pn:g} / 段 {period}（{note}）"
                try:
                    p, _, runs, frac, _ = step_runs(olp, path, mode, period)
                    got = p / frac
                    err = None
                    if tol is None:
                        if got < period:
                            err = f"供給周期 {got:.3f} が段 {period} を下回った"
                    elif abs(got - pn) / pn > tol:
                        err = f"供給周期 {got:.3f} (期待 {pn:g})"
                    elif pn == 16.0 and any(r % 2 for r in runs):
                        # ちょうど 2 倍でロックしていれば、値が変わるのは 1 段おき
                        # だけ。閾値は独立ペアの下側 10% 点なので真の変化の 1 割は
                        # 取りこぼすが、そのぶんは連が 4, 6 と伸びるだけで、
                        # **奇数の連は 1 本も出ない**のが 2:1 ロックの署名になる。
                        odd = {r: n for r, n in sorted(Counter(runs).items())
                               if r % 2}
                        err = f"奇数の連が出た: {dict(list(odd.items())[:4])}"
                except Exception as e:            # noqa: BLE001
                    err = str(e)
                results.append((name, err))

    ng = 0
    for name, err in results:
        if err is None:
            print(f"PASS {name}")
        else:
            print(f"FAIL {name}: {err}")
            ng += 1
    print(f"--- {len(results)} ケース / NG {ng}")
    return 1 if ng else 0


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
    parser.add_argument("--runs", type=Path, nargs="+", metavar="WAV",
                        help="段ごとの LFO 値を取り出して連の長さを数える"
                             "（--mode が要る）")
    parser.add_argument("--period", type=float, default=None,
                        help="--runs で使う更新周期 [samples]。省略すると推定する")
    parser.add_argument("--supply", action="store_true",
                        help="[J] --wav-dir の全条件で NFRQ ごとの供給周期を集計する")
    parser.add_argument("--self-test", action="store_true",
                        help="--runs を人工信号で自己検証する（実機不要）")
    parser.add_argument("--mode", choices=MODES,
                        help="--gaps / --runs で使う解析モード")
    parser.add_argument("--quiet", action="store_true",
                        help="解析器の警告を捨てる")
    args = parser.parse_args(argv)

    if not ANALYZER.exists():
        print(f"! 解析器が見つからない: {ANALYZER}", file=sys.stderr)
        return 1

    if args.self_test:
        return self_test()

    if args.gaps:
        if not args.mode:
            parser.error("--gaps には --mode が要る")
        report_gaps(args.gaps, args.mode)
        return 0

    if args.runs:
        if not args.mode:
            parser.error("--runs には --mode が要る")
        report_runs(args.runs, args.mode, args.period)
        return 0

    if args.supply:
        report_supply(args.wav_dir)
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
