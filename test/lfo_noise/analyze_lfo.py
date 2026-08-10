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
./analyze_lfo.py --self-test          # --runs / --values を人工信号で検証（実機不要）

# 個別条件の中身を見る: 更新イベントの間隔が更新周期の整数倍に揃っているかを直接確かめる
./analyze_lfo.py --mode am --gaps wav/am_nfrq_1f_lfrq_2{0,4,8,f}_kc_4a_mul_04.wav.zst

# 段ごとの LFO 値を取り出し、同じ語を続けて引いた回数から値の供給周期を出す
./analyze_lfo.py --mode am --period 8 --runs wav_nfrq_5a09/am_nfrq_00_lfrq_ff_*.wav.zst

# 値列そのものを見て、キャプチャ間・条件間で突き合わせる
./analyze_lfo.py --mode am --values wav_value/am_nfrq_00_lfrq_a{0,8}_*.wav.zst
./analyze_lfo.py --mode am --period 65536 --dump 24 \
    --values wav_lfrq28/am_nfrq_1f_lfrq_28_*.wav.zst
```

`--gaps` は集計ではなく**個別条件の生の中身**を見るためのもの。イベント間隔が推定周期の
整数倍に揃っていれば「更新周期は一定で、値が変わらなかった回がある」と読める。
`[B]` の外れ値がどこから来ているかはこれで確かめる。

`--runs` は更新の**タイミング**ではなく**値そのもの**を見る。`--period` で更新周期を
渡せるのが要で、値が 1 回おきにしか変わらない条件（`NFRQ=0x00` の `f` 帯）でも
「LFO が本当は何サンプルごとにラッチしているか」を仮定して問い直せる。

`--values` はその値を**連に畳まずに列のまま**扱う。`--runs` が答えられない
「別のキャプチャ / 別の条件で**同じ語の列**を引いているか」を、遅れを総当たりして
突き合わせる。順位に直して比べるので、モードが違ってもゲインがずれても効く。

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
# changed_fraction が要求する最小の段数と、独立ペアを取る間隔
BLOCKS_MIN = 256
FAR_STEP = 7

# --values で無関係な 2 列が偶然一致してしまう確率。一致の閾値をここから決める
VALUE_CHANCE = 0.10
# --values の遅れ探索の既定幅 [段] と、突き合わせに要求する重なりの段数
VALUE_LAG = 256
VALUE_OVERLAP_MIN = 64
# --values で統計に要求する最小の段数。段数が数百本しか取れない条件
# （P が大きいキャプチャ）でも値列を出せるように BLOCKS_MIN より下げる
VALUE_BLOCKS_MIN = 32
# --values で格子の位相を総当たりする更新周期の上限 [samples]。これより長い段では
# 位相のずれ（最大でも 1 段の頭 P サンプル）が段の長さに対して十分小さく、
# 隣の語が混ざらないので探索が要らない
VALUE_PHASE_MAX = 64
# --values で独立ペアをこの本数くらい取るように間隔を決める
VALUE_FAR_PAIRS = 4096
# --values で段の末尾も捨てる更新周期の下限 [samples]。これより短い段では
# 捨てる余地が無い（P=8 なら頭 2 + 尻 2 で 4 サンプルしか残らない）
VALUE_TAIL_MIN = 16
# --values の「偶然の上限」を、外れた遅れでの一致率の分布のどこに取るか（上側）
VALUE_LUCK = 0.99
# --values で「一致」と言うのに要求する真の一致の下限。上限を 3σ 超えていても
# 真の一致が数 % では列が合っているとは言えない
VALUE_MATCH_MIN = 0.15


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


def block_values(a, mode, period, off, guard=BLOCK_GUARD, tail=0):
    """ラッチ格子 [off + iP + guard, off + (i+1)P - tail) ごとの LFO 値を返す。

    `build_sums` が作った累積和からブロックの値を復元するだけ。AM は
    `0.5 log Σenv²`（= 対数振幅）、PM は最小二乗の cos(ω)（= 搬送波の周波数）で、
    どちらもその段の LFO 値の単調関数になる。`level_feature` はこの 1 階差分しか
    返さないので、値そのものはここで作る。

    `guard` は段の頭を捨てる幅。包絡も cos(ω) も 3 サンプルのカーネル
    （`x[j-1], x[j], x[j+1]`）で作るので、段の境界をまたぐ 2 サンプルは前後の段の
    値が混ざる。P=8 では 8 サンプル中 2 なので効きが大きく、捨てないと隣り合う段の
    値が引き寄せ合って「変わっていない」側に倒れる。

    `tail` は段の末尾を捨てる幅で、既定は 0（`[J]` を出したときと同じ）。
    **AM では次の語との振幅の段差でカーネルが跳ね、その大きさが段差の位置での
    搬送波位相に依存する。**同じ語でもキャプチャが違えば位相が違うので、
    値をキャプチャ間で突き合わせるときはここも捨てる（`--values`）。
    段が短い条件では捨てる余地が無いので既定は 0 のままにしてある。
    """
    nb = (a.m - off) // period
    if nb < 4 or period - guard - tail < 2:
        return []
    if mode == "am":
        acc = a.sums[0]
        out = []
        for i in range(nb):
            v = (acc[off + (i + 1) * period - tail]
                 - acc[off + i * period + guard])
            out.append(0.5 * math.log(v) if v > 0.0 else 0.0)
        return out
    p, q = a.sums
    out = []
    for i in range(nb):
        lo, hi = off + i * period + guard, off + (i + 1) * period - tail
        den = q[hi] - q[lo]
        out.append((p[hi] - p[lo]) / (2.0 * den) if den > 0.0 else 0.0)
    return out


def changed_fraction(v, min_blocks=BLOCKS_MIN, far_step=FAR_STEP):
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

    `min_blocks` / `far_step` は既定のままなら `[J]` を出したときと同じ挙動。
    段数が少ない条件（`P` が大きいキャプチャ）で `--values` から呼ぶときだけ、
    要求段数を下げて独立ペアを密に取る。
    """
    n = len(v)
    if n < min_blocks:
        return None, None
    d1 = sorted(abs(b - c) for c, b in zip(v, v[1:]))
    far = sorted(abs(v[i + k] - v[i])
                 for k in range(5, 21) for i in range(0, n - k, far_step))
    if len(far) < min_blocks:
        return None, None
    t = far[int(len(far) * FAR_Q)]
    if t <= 0.0:
        return None, None
    f1 = bisect.bisect_left(d1, t) / len(d1)
    c = (1.0 - f1) / (1.0 - FAR_Q)
    return min(1.0, max(1e-6, c)), t


def align_values(a, mode, p, offsets=None, min_blocks=BLOCKS_MIN,
                 far_step=FAR_STEP, tail=0):
    """格子の位相を決めて (位相, 値列, 変化した割合 c, 閾値 t) を返す。

    位相は総当たりで決める（候補は p 通りしかない）。ずれていると 1 つの
    ブロックが 2 つの語にまたがり、変わっていない段まで変わって見えるので、
    **c が最小になる位相**が正しい整列。`offsets` に候補を絞って渡せば
    そのなかから選ぶ（段が長ければ位相はほとんど効かないので 1 通りで足りる）。
    """
    best = None
    for off in (range(p) if offsets is None else offsets):
        v = block_values(a, mode, p, off, tail=tail)
        c, t = changed_fraction(v, min_blocks, far_step)
        if c is None:
            continue
        if best is None or c < best[0]:
            best = (c, t, off, v)
    if best is None:
        raise ValueError("段が足りない")
    c, t, off, v = best
    return off, v, c, t


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

    off, v, frac, thr = align_values(a, mode, p)

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


# ---- 段ごとの値列そのものを見る（--values） ----------------------------

def value_series(olp, path, mode, period=None):
    """1 条件の段ごとの LFO 値列を取り出す。

    返すのは (更新周期, 位相, 値列, 変化した割合 c, 閾値 t)。`--runs` と違って
    連の長さには畳まず、**値列そのもの**を返す。段数が少ない条件（`P` が大きい
    キャプチャ）でも使えるように、統計に要求する段数を下げ、独立ペアを密に取る。
    """
    x, _ = olp.load_left(path)
    a = olp.Analyzer(x, mode)
    if period is None:
        period, _ = a.run(lambda msg: None)
    p = int(round(period))
    if p < 2 or abs(period - p) > 0.05 * p:
        raise ValueError(f"格子にできる整数周期でない: {period:.3f}")

    nb = max(1, a.m // p)
    far_step = max(1, nb * 16 // VALUE_FAR_PAIRS)
    # 段が長ければ位相のずれは段の長さに対して十分小さく、隣の語が混ざらない。
    # 総当たりは段が短いときだけやる（P=256 で 256 通り × 全段の統計は重い）
    offsets = None if p <= VALUE_PHASE_MAX else (0,)
    tail = BLOCK_GUARD if p >= VALUE_TAIL_MIN else 0
    off, v, c, t = align_values(a, mode, p, offsets, VALUE_BLOCKS_MIN,
                                far_step, tail)
    return p, off, v, c, t


def value_center(v):
    """値列から中央値を引く。

    AM の値は `0.5 log Σenv²` なので、キャプチャ間でゲインが g 倍ずれると
    `log g` の**定数**だけ平行移動する。中央値を引けばこれが落ちる。
    順位に直す手もあるが、順位は母集団が変われば `1/sqrt(n)` のオーダーで動き、
    それが下の閾値と同じ大きさになってしまうので使わない。
    """
    s = sorted(v)
    med = s[len(s) // 2]
    return [x - med for x in v]


def match_threshold(va, vb):
    """「無関係な段どうし」の |差| の下側 VALUE_CHANCE 点を閾値にする。

    こう決めると **無関係な 2 列なら一致率がちょうど VALUE_CHANCE になる**ので、
    そこからの上振れがそのまま「本当に同じ語だった割合」になる。閾値を手で
    与えない決め方は changed_fraction と同じ考え方。

    無関係なペアは、片方を列の 1/3・1/2・2/3 だけ巡回させて作る。ノイズ語は
    互いに独立なので、この程度ずらせば必ず別の語どうしになる。
    """
    n = min(len(va), len(vb))
    if n < VALUE_OVERLAP_MIN:
        return None
    far = sorted(abs(va[(i + s) % n] - vb[i])
                 for s in (n // 3, n // 2, 2 * n // 3) for i in range(n))
    t = far[int(len(far) * VALUE_CHANCE)]
    return t if t > 0.0 else None


def match_rate(va, vb, lag, t):
    """遅れ lag での一致率と、重なった段数。重ならなければ (None, 0)。"""
    lo, hi = max(0, -lag), min(len(vb), len(va) - lag)
    n = hi - lo
    if n < VALUE_OVERLAP_MIN:
        return None, 0
    m = sum(1 for i in range(lo, hi) if abs(va[i + lag] - vb[i]) < t)
    return m / n, n


def best_match(va, vb, span):
    """一致率が最大になる遅れを総当たりで探す。

    返すのは (一致率, 遅れ, 段数, 偶然の上限)。実機はハードウェアリセットから
    レジスタ書き込みまでの時間がホスト側の都合で揺れるので、2 本のキャプチャで
    値列の開始位置が揃う保証が無い。定数の遅れを許さないと「列が合っているか
    どうか」自体を判定できない。

    **総当たりの最大値には選択バイアスが乗る。**遅れを数百通り試せば、無関係な
    2 列でもどれかは偶然よく一致する。そこで外れた遅れでの一致率の分布から
    上側 VALUE_LUCK 点を「偶然の上限」として一緒に返す。**最良がこれを超えて
    いなければ、その一致は偶然と区別が付かない。**
    探索幅は段数の 1/4 で頭打ちにする（候補が多いほど上限が上がるだけなので）。
    """
    t = match_threshold(va, vb)
    if t is None:
        return (None, 0, 0, None)
    n_all = min(len(va), len(vb))
    span = max(1, min(span, n_all // 4))
    rates = []
    best = (None, 0, 0)
    for lag in range(-span, span + 1):
        r, n = match_rate(va, vb, lag, t)
        if r is None:
            continue
        rates.append((r, lag))
        if best[0] is None or r > best[0]:
            best = (r, lag, n)
    if best[0] is None:
        return (None, 0, 0, None)
    # 最良の遅れの近傍は本物の一致なら道連れで高くなるので、上限の推定から外す
    other = sorted(r for r, lag in rates if abs(lag - best[1]) > 1)
    luck = other[int(len(other) * VALUE_LUCK)] if other else None
    return best + (luck,)


def true_fraction(rate):
    """一致率から「本当に同じ語だった段の割合」を出す。

    無関係な 2 列でも順位差の閾値の内側に入る確率が VALUE_CHANCE あるので、
    その下駄を外す。1.0 なら完全一致、0.0 なら無関係。
    """
    return (rate - VALUE_CHANCE) / (1.0 - VALUE_CHANCE)


def report_values(paths, mode, period=None, span=VALUE_LAG, dump=0):
    """段ごとの値列を出し、2 本目以降を 1 本目と突き合わせる。"""
    olp = load_analyzer()
    section(f"段ごとの LFO 値列（--mode {mode}）")
    print("ラッチ格子ごとの LFO 値を取り出し、連に畳まず値列のまま見る。")
    print("  段境界の変化率を偶数・奇数に分けて出す。")
    print("    片方だけ高い -> 1 段おきにしか変わっていない（同じ語を 2 回引いている）")
    print("    両方とも同じ -> どの段でも変わりうる")
    print("  突き合わせの一致率は無関係な 2 列でも "
          f"{VALUE_CHANCE * 100:.0f}% 出る。下駄を外したものが「真の一致」。")
    print("  遅れは総当たりなので最良値には選択バイアスが乗る。")
    print("  **「偶然の上限」を超えていない一致率は偶然と区別が付かない。**")
    print()

    got = []
    for path in paths:
        # 同じ条件を別のディレクトリで撮り直したものを並べるので、親も出す
        name = f"{Path(path).parent.name}/{Path(path).name}"
        try:
            p, off, v, c, t = value_series(olp, path, mode, period)
        except Exception as e:                        # noqa: BLE001
            print(f"* {name}: {e}")
            continue
        nb = len(v) - 1
        chg = [abs(x1 - x0) >= t for x0, x1 in zip(v, v[1:])]
        ne = sum(1 for i in range(0, nb, 2) if chg[i])
        no = sum(1 for i in range(1, nb, 2) if chg[i])
        de, do = max(1, len(range(0, nb, 2))), max(1, len(range(1, nb, 2)))
        print(f"* {name}")
        print(f"    更新周期 {p} samples / 位相 {off} / 段 {len(v)} / "
              f"変化した段 {c * 100:.1f}%")
        print(f"    段境界の変化率: 偶数 {ne}/{de} = {ne / de * 100:.0f}% / "
              f"奇数 {no}/{do} = {no / do * 100:.0f}%")
        if dump:
            order = sorted(range(len(v)), key=lambda i: v[i])
            rank = [0] * len(v)
            for k, i in enumerate(order):
                rank[i] = round(k * 255 / max(1, len(v) - 1))
            print(f"    値列（順位を 0-255 に直したもの、先頭 {dump} 段）:")
            print("     " + "".join(f"{r:4d}" for r in rank[:dump]))
        got.append((name, value_center(v)))

    if len(got) < 2:
        return
    print()
    print("  突き合わせ（1 本目を基準）:")
    print("  相手                                          遅れ   一致率"
          "  偶然の上限   真の一致  重なり  判定")
    base = got[0][1]
    for name, vb in got[1:]:
        rate, lag, n, luck = best_match(base, vb, span)
        if rate is None:
            print(f"  {name[:44]:44s}     -       -           -         -"
                  "       0  -")
            continue
        # 偶然の上限を 3σ 超えたときだけ「一致」と言う。上限そのものも
        # 有限個の遅れから推定した値なので、ぎりぎり超えただけでは足りない
        ok = "偶然と同程度"
        if luck is not None:
            sigma = math.sqrt(max(luck * (1.0 - luck), 1e-9) / n)
            if rate > luck + 3.0 * sigma \
                    and true_fraction(rate) >= VALUE_MATCH_MIN:
                ok = "一致"
        lk = f"{luck * 100:6.1f}%" if luck is not None else "     -"
        print(f"  {name[:44]:44s} {lag:5d}  {rate * 100:6.1f}%      {lk}  "
              f"{true_fraction(rate) * 100:7.1f}%  {n:6d}  {ok}")


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
# --values の「真の一致」に許す絶対誤差。段数 600 の標本ゆらぎ（約 2%）と
# 値の取り出し誤差を見込む
VALUE_TEST_TOL = 0.10


def self_test_values(olp, tg, tmp):
    """既知の値列を入れた人工信号で `--values` を検証する（実機不要）。

    `synth(..., values=)` に列を直接渡せるので、「同じ列」「ずらした列」
    「既知の割合だけ差し替えた列」「無関係な列」を作って、突き合わせが
    その割合を戻すかを見る。最後の 1 件は 1-c で使う判別器
    （1 段おきにしか変わらないと境界のパリティが片側に寄る）の検証。
    """
    period, nseg = 256, 600
    length = period * nseg
    nval = nseg + 2
    base = tg.lfo_values_iid(nval, 4200)
    other = tg.lfo_values_iid(nval, 4201)
    # 1 段おきに差し替える。残る一致は 50%
    half = [other[i] if i % 2 else base[i] for i in range(nval)]

    # (名前, B の値列, B の遅れ, 期待する真の一致)
    cases = (
        ("同じ列", base, 0, 1.0),
        ("5 段ずらした列", base[5:] + base[:5], 5, 1.0),
        ("1 段おきに差し替えた列", half, 0, 0.5),
        ("無関係な列", other, 0, 0.0),
    )

    results = []
    for mode in MODES:
        pa = Path(tmp) / f"{mode}_values_a.wav"
        if not pa.exists():
            tg.write_wav(pa, tg.synth(mode, period, length, tg.CARRIERS[2],
                                      0, values=base))
        _, _, va, _, _ = value_series(olp, pa, mode, period)
        va = value_center(va)
        for note, vals, lag_want, want in cases:
            pb = Path(tmp) / f"{mode}_values_{note}.wav"
            if not pb.exists():
                tg.write_wav(pb, tg.synth(mode, period, length, tg.CARRIERS[2],
                                          0, values=vals))
            name = f"{mode} 値列の突き合わせ（{note}）"
            err = None
            try:
                _, _, vb, _, _ = value_series(olp, pb, mode, period)
                rate, lag, _, _ = best_match(va, value_center(vb), 32)
                got = true_fraction(rate) if rate is not None else None
                if got is None:
                    err = "突き合わせできない"
                elif abs(got - want) > VALUE_TEST_TOL:
                    err = f"真の一致 {got * 100:.1f}% (期待 {want * 100:.0f}%)"
                elif want > 0.9 and lag != lag_want:
                    err = f"遅れ {lag} (期待 {lag_want})"
            except Exception as e:                    # noqa: BLE001
                err = str(e)
            results.append((name, err))

        # 1 段おきにしか変わらない列。境界のパリティが片側に寄るのが署名
        pp = Path(tmp) / f"{mode}_values_pair.wav"
        if not pp.exists():
            tg.write_wav(pp, tg.synth(mode, period, length, tg.CARRIERS[2],
                                      4300, noise_period=2 * period, iid=True))
        name = f"{mode} 同じ語を 2 回引く列（境界のパリティ）"
        err = None
        try:
            _, _, v, _, t = value_series(olp, pp, mode, period)
            at = [i for i, (x0, x1) in enumerate(zip(v, v[1:]))
                  if abs(x1 - x0) >= t]
            even = sum(1 for i in at if i % 2 == 0)
            odd = len(at) - even
            if min(even, odd) > 0.05 * max(even, odd):
                err = f"パリティが偏らない: 偶数 {even} / 奇数 {odd}"
        except Exception as e:                        # noqa: BLE001
            err = str(e)
        results.append((name, err))
    return results


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

        results += self_test_values(olp, tg, tmp)

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
    parser.add_argument("--values", type=Path, nargs="+", metavar="WAV",
                        help="段ごとの LFO 値列を出し、2 本目以降を 1 本目と"
                             "突き合わせる（--mode が要る）")
    parser.add_argument("--dump", type=int, default=0, metavar="N",
                        help="--values で値列の先頭 N 段を表示する")
    parser.add_argument("--lag-span", type=int, default=VALUE_LAG, metavar="N",
                        help=f"--values で探す遅れの幅 [段]（既定 {VALUE_LAG}）")
    parser.add_argument("--period", type=float, default=None,
                        help="--runs / --values で使う更新周期 [samples]。"
                             "省略すると推定する")
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

    if args.values:
        if not args.mode:
            parser.error("--values には --mode が要る")
        report_values(args.values, args.mode, args.period,
                      args.lag_span, args.dump)
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
