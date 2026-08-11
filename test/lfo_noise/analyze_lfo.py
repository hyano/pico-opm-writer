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
    [N] W≠3（鋸・三角）の段の間隔（--stair）
    [P] 2 値化した値列の突き合わせ（--bits）
    [R] 鋸 (W=0) の 1 周期あたりの段数と歩幅（--cycle）

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

# W≠3（鋸・三角）の段の間隔を測り、実機モデルと ymfm モデルを判別する
./analyze_lfo.py --mode am --stair wav_w0/am_*.wav.zst

# 値列を 2 値化し、遅れを全域で探して総当たりで突き合わせる（同じ列どうしを組にする）
./analyze_lfo.py --mode am --period 256 --bits wav_value{,_r2,_r3}/am_*_lfrq_a?_*.wav.zst

# 鋸 (W=0) の 1 周期の段数を数えて歩幅を出す
./analyze_lfo.py --mode am --cycle wav_w0/am_*.wav.zst
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

`--bits` は `--values` の 2 点を差し替えたもの。値を**中央値で 2 値化**するので
キャプチャ間のゲインのずれに左右されず、**遅れを全域で探す**ので開始位置が数百段
ずれていても見つかる。`--values` の探索幅（256 段）ではこれを取り逃がしていた
（README §5.9）。

`--stair` は W≠3 専用。鋸・三角では LFO 値が単調に動くので、値が独立であることを
前提にする `--runs` は使えない。代わりに**階段の段差から段の間隔を直接測る**。
下位ニブルが「更新の間隔」を変えるのか「1 回の更新で進む歩幅」を変えるのかが
これで判別できる（README §5.7）。

`--cycle` はその後半、**歩幅そのもの**を数値にする。鋸の折り返しで 1 周期を切り出し、
そのあいだの段数を数えると平均の歩幅が `256 / 段数` として出る（README §5.8）。

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

# --bits で「同じ値列」と言える一致率の下限。2 値化した列どうしのハミング距離は
# 無関係なら 50%、同じ列なら値の取り出し誤差ぶんだけ 100% から落ちる（実測 99% 台）。
# 両者の隔たりが大きいので、閾値の置き場所は結論を変えない
BITS_MATCH = 0.90
# --bits の遅れ探索の既定幅 [段]。**--values の VALUE_LAG より桁で広く取る。**
# リセットからキャプチャ開始までの揺れは段の数百本ぶんに達しうる
BITS_LAG = 2048
# --bits が突き合わせに要求する重なりの段数
BITS_OVERLAP_MIN = 400
# --bits の「偶然の上限」を、外れた遅れでの一致率の分布のどこに取るか（上側）
BITS_LUCK = 0.99

# --cycle で折り返しと見なす段差の、通常の段差の中央値に対する比。鋸の折り返しは
# 全振幅ぶん動くので 1 段ぶんの数十倍になる。8 倍は十分に余裕のある置き場所
CYCLE_WRAP_RATIO = 8.0
# --cycle で折り返しの滲みを 1 本にまとめる幅 [block]
CYCLE_WRAP_MERGE = 4
# --cycle が捨てるキャプチャ頭のブロック数（KEY ON の立ち上がりを折り返しと
# 取り違えないため）
CYCLE_SKIP = 32
# --cycle で「2 段ぶんの段差」と見なす、1 段ぶんの段差に対する比
CYCLE_BIG_RATIO = 1.5
# --cycle が段差の大小を出すのに要求する「1 段あたりのブロック数」。これを
# 切ると段差がブロックの平均に丸められ、+1 と +2 を区別できなくなる
CYCLE_BIG_MIN = 16

# --stair で 1 段を何ブロックに割るか。段の間隔の分解能が ±(段/この値) になる
STAIR_SUB = 8
# --stair のブロック長の下限 [samples]。これより短いと 1 ブロックの平均が効かず、
# 段差（対数振幅で 0.01 前後）が包絡の雑音に埋まる
STAIR_GRID_MIN = 16
# --stair で段の境界と見なす閾値。ブロック間差分の絶対値の上位 10% 点に対する比。
# **1 段ぶんの差分（+1）も 2 段ぶん（+2）も拾える位置**に置く必要がある。
# 下位ニブルが大きい帯では歩幅が +1 と +2 で交互になるので、閾値が両者の間に
# 落ちると片方だけを数えて間隔が 2 倍に出る
STAIR_THR = 0.4
# --stair が間隔を出すのに要求する境界の数
STAIR_EDGES_MIN = 5
# --stair が要求する「1 段あたりのブロック数」の下限。**これが 2 だと結論が歪む**:
# 滲みをまとめる規則（続いた検出を 1 本にする）が、真の間隔によらず 1 つおきに
# 境界を落とすので、間隔が必ず 2 ブロックと出てしまう。3 以上あればまとめる規則が
# 効くのは本当に滲んだ場合だけになる
STAIR_GAP_MIN = 3


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

    **キャプチャどうしを突き合わせるときは位相を合わせないといけない**（格子が
    段の境界からずれると 1 ブロックが 2 つの語にまたがって値が混ざる）。それを
    やるのは `bits_series`（`--bits`）で、位相の決め方もそちらは別にしてある。
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


# ---- 追加の取り込みが入る位置の予測（--bits / --values の照合用） -----------
#
# Nuked-OPM `opm.c:1607-1622`（if/else 連鎖）と YM2151-LLE `fmopm.c:1001-1010`
# （OR 形式。条件が排他なので等価）が同じデコード論理を持つ。粗い更新の回数を
# 数える 4bit カウンタ `cnt` を見て、LFRQ の下位ニブルの各ビットが立っていれば
# 追加のパルスを 1 発差し込む。**本レポートの実測ではなく、エミュレータ実装から
# 読んだ仮説**であることに注意（README §6.3）。

def extra_pulse(cnt, lo):
    """粗い更新 `cnt` 回目に追加のパルスが入るか（デコード表）。"""
    return bool((cnt % 2 == 0 and lo & 8)
                or (cnt & 3) == 1 and lo & 4
                or (cnt & 7) == 3 and lo & 2
                or (cnt & 15) == 7 and lo & 1)


def pair_positions(lo):
    """値が変わらない段境界の位置（`cnt` の 0-15）。

    追加のパルスがある段では取り込みが 2 回起き、観測に残るのは 2 回目の語。
    段 `k` の 2 回目と段 `k+1` の 1 回目が同じ語になる条件が満たされていれば
    （README §6.2 の条件 1・2）、**「追加あり」の直後に「追加なし」が来る境界**
    だけが「値が変わらない境界」になる。
    """
    return [k for k in range(16)
            if extra_pulse(k, lo) and not extra_pulse((k + 1) % 16, lo)]


def pair_fraction(lo):
    """値が変わらない段境界の割合。デコード表から `min(lo, 16-lo) / 16` になる。"""
    return len(pair_positions(lo)) / 16.0


def pair_parity(lo):
    """値が変わらない段境界が寄るパリティ。`lo<=7` は奇数 / `lo>=8` は偶数。

    **どちらの側が高く出るかは格子の切り方で決まるので、絶対的なラベルには
    意味がない。**意味があるのは `lo` を振ったときに反転する位置の方で、
    デコード表はそれを `7` と `8` の間だと予測する。
    """
    par = {k % 2 for k in pair_positions(lo)}
    return {frozenset(): "-", frozenset({0}): "偶数",
            frozenset({1}): "奇数"}.get(frozenset(par), "混在")


# ---- 2 値化した値列の突き合わせ（--bits） --------------------------------

def series_bits(v):
    """値列を中央値で 2 値化する。

    `--values` は値の差を閾値と比べるので、キャプチャ間でゲインがずれると
    その差そのものが動く。**中央値で 2 値化してハミング距離で比べれば、
    値を単調に歪める要因（ゲイン・オフセット・対数の底）が全部落ちる。**
    落とすのは分解能で、無関係な 2 列の一致率が 50% に上がる代わりに、
    同じ列なら値の取り出し誤差ぶんしか落ちない（実機で 99% 台）。

    LFO のノイズ値は 0-255 がほぼ一様なので、中央値での分割は
    **値の最上位ビット**を取り出すことに当たる。
    """
    s = sorted(v)
    med = s[len(s) // 2]
    return [1 if x >= med else 0 for x in v]


def bits_series(olp, path, mode, period=None):
    """`--bits` 用に、格子の位相を合わせた段ごとの値列を取り出す。

    返すのは (更新周期, 位相, 値列)。

    位相の決め方が [§4.7](README.md) の `align_values` と違う。あちらは
    **変化した段の割合 `c` が最小になる位相**を採るが、これは値列の構造に
    引きずられる（1 段おきにしか変わらない列では、ずれた位相の方が `c` を
    小さくできてしまう）。ここでは代わりに **ブロック値の散らばりが最大に
    なる位相**を採る。格子が段の境界からずれると 1 ブロックが 2 つの語の
    混合になり、値は必ず中央へ寄るので、散らばりが最大なのが混ざっていない
    位相になる。**この判定は値列の構造に依らない。**
    """
    x, _ = olp.load_left(path)
    a = olp.Analyzer(x, mode)
    if period is None:
        period, _ = a.run(lambda msg: None)
    p = int(round(period))
    if p < 2 or abs(period - p) > 0.05 * p:
        raise ValueError(f"格子にできる整数周期でない: {period:.3f}")
    tail = BLOCK_GUARD if p >= VALUE_TAIL_MIN else 0

    best = None
    for off in range(p):
        v = block_values(a, mode, p, off, tail=tail)
        if len(v) < VALUE_BLOCKS_MIN:
            continue
        s = sorted(v)
        # 外れ値に引きずられないように 10-90% 点の幅で散らばりを測る
        spread = s[int(len(s) * 0.9)] - s[int(len(s) * 0.1)]
        if best is None or spread > best[0]:
            best = (spread, off, v)
    if best is None:
        raise ValueError("段が足りない")
    return p, best[1], best[2]


def pack_bits(b):
    """2 値列を 1 個の整数に詰める。先頭の段が最上位ビット。"""
    n = 0
    for x in b:
        n = (n << 1) | x
    return n, len(b)


def _popcount(n):
    try:
        return n.bit_count()                       # Python 3.10 以降
    except AttributeError:                         # pragma: no cover
        return bin(n).count("1")


def bit_match(pa, pb, span=BITS_LAG, minov=BITS_OVERLAP_MIN):
    """2 値列どうしの一致率が最大になる遅れを総当たりで探す。

    返すのは (遅れ, 一致率, 重なった段数, 偶然の上限)。整数に詰めて
    XOR + popcount で比べるので、遅れを数千通り試しても実用的な速さになる。
    **これが要点で、`--values` の探索幅（VALUE_LAG）では足りない。**実機は
    リセットからレジスタ書き込みまでの時間がホスト側の都合で揺れ、その揺れは
    段の数百本ぶんに達しうる。狭い幅で探すと「一致しない」と「探し損ねた」を
    区別できない。
    """
    # 遅れの向きは `best_match` に合わせる（**pa[i+lag] と pb[i] を比べる**）。
    # 中の畳み込みは「第 1 引数の i と 第 2 引数の i+lag」なので、引数を
    # 入れ替えて回すとこの向きになる
    X, nx = pb
    Y, ny = pa
    best = (None, 0.0, 0)
    rates = []
    lo = max(-span, -(ny - minov))
    hi = min(span, nx - minov)
    for lag in range(lo, hi + 1):
        s, e = max(0, -lag), min(nx, ny - lag)
        n = e - s
        if n < minov:
            continue
        mask = (1 << n) - 1
        xs = (X >> (nx - e)) & mask
        ys = (Y >> (ny - (e + lag))) & mask
        r = 1.0 - _popcount(xs ^ ys) / n
        rates.append((r, lag))
        if r > best[1]:
            best = (lag, r, n)
    if best[0] is None:
        return (None, 0.0, 0, None)
    other = sorted(r for r, lag in rates if abs(lag - best[0]) > 1)
    luck = other[int(len(other) * BITS_LUCK)] if other else None
    return best + (luck,)


def report_bits(paths, mode, period=None, span=BITS_LAG):
    """[P] 2 値化した値列を総当たりで突き合わせ、同じ列どうしを組にまとめる。

    `--values` が答えられなかった「中間の下位ニブルが同じ値列を出すのか」を
    決めるためのもの。`--values` との違いは 2 点だけで、どちらも
    **条件の効果と測り方の限界を分けるため**に入れてある。

    1. 値を中央値で 2 値化する（ゲインのずれに左右されない）
    2. 遅れを全域で探す（`--values` の 256 段では足りない）
    3. 格子の位相を、値列の構造に依らない基準で合わせる（`bits_series`）

    出力は 1 本ずつの素性と、`BITS_MATCH` 以上で結ばれる組を連結成分に
    まとめたもの。**同じ下位ニブルどうしが同じ成分に入りやすいかどうか**が
    「値列が下位ニブルで決まるか」の判別になる。
    """
    olp = load_analyzer()
    section(f"[P] 2 値化した値列の突き合わせ（--mode {mode}）")
    print("段ごとの LFO 値を中央値で 2 値化し、遅れを全域で探して突き合わせる。")
    print("  無関係な 2 列なら一致率は 50%、同じ列なら値の取り出し誤差ぶんしか")
    print("  落ちない。閾値は "
          f"{BITS_MATCH * 100:.0f}% で、両者の隔たりが大きいので置き場所は結論を変えない。")
    print(f"  遅れの探索幅 ±{span} 段（--values の {VALUE_LAG} 段では足りない）。")
    print()

    got = []
    for path in sorted(paths):
        name = f"{Path(path).parent.name}/{Path(path).name}"
        m = NAME_RE.search(Path(path).name)
        key = m[2] if m else "??"
        try:
            p, off, v = bits_series(olp, path, mode, period)
        except Exception as e:                        # noqa: BLE001
            print(f"* {name}: {e}")
            continue
        b = series_bits(v)
        got.append((name, key, pack_bits(b)))
        print(f"* {name}")
        print(f"    更新周期 {p} samples / 位相 {off} / 段 {len(v)} / "
              f"1 の割合 {sum(b) / len(b) * 100:.1f}%")

    n = len(got)
    if n < 2:
        return
    print()
    print(f"  突き合わせ（{n} 本の総当たり {n * (n - 1) // 2} 組）:")
    adj = [set() for _ in range(n)]
    luckmax = 0.0
    hit, miss, lags = [], [], []
    for i in range(n):
        for j in range(i + 1, n):
            lag, r, ov, luck = bit_match(got[i][2], got[j][2], span)
            if lag is None:
                continue
            luckmax = max(luckmax, luck or 0.0)
            if r >= BITS_MATCH:
                adj[i].add(j)
                adj[j].add(i)
                hit.append(r)
                lags.append(lag)
            else:
                miss.append(r)
    print(f"    偶然の上限（外れた遅れでの一致率の上側 "
          f"{BITS_LUCK * 100:.0f}% 点）の最大 {luckmax * 100:.1f}%")
    # **一致した組と外れた組が分離しているか**が読みどころ。閾値の近くに
    # 溜まっていれば閾値の置き方が結論を左右していることになる
    if hit:
        print(f"    閾値以上の {len(hit)} 組: 一致率 {min(hit) * 100:.1f}"
              f"〜{max(hit) * 100:.1f}% / 遅れ {min(lags)}〜{max(lags)} 段")
    if miss:
        print(f"    閾値未満の {len(miss)} 組: 一致率 {min(miss) * 100:.1f}"
              f"〜{max(miss) * 100:.1f}%")

    seen, comps = set(), []
    for i in range(n):
        if i in seen:
            continue
        stack, comp = [i], []
        while stack:
            u = stack.pop()
            if u in seen:
                continue
            seen.add(u)
            comp.append(u)
            stack.extend(adj[u] - seen)
        comps.append(sorted(comp))
    comps.sort(key=len, reverse=True)

    print()
    print("  同じ値列でまとまった組（連結成分）:")
    print("  組   本数  LFRQ の種類  内訳")
    for ci, comp in enumerate(comps, 1):
        keys = sorted({got[k][1] for k in comp})
        print(f"  {ci:2d}  {len(comp):5d}  {len(keys):9d}    {' '.join(keys)}")

    # 「値列が LFRQ で決まる」なら、同じ LFRQ どうしが同じ組に入りやすいはず
    cl = {k: ci for ci, comp in enumerate(comps) for k in comp}
    ss = sh = ds = dh = 0
    for i in range(n):
        for j in range(i + 1, n):
            same = got[i][1] == got[j][1]
            hit = cl[i] == cl[j]
            if same:
                ss += 1
                sh += hit
            else:
                ds += 1
                dh += hit
    print()
    print("  同じ組に入る確率:")
    print(f"    同じ LFRQ どうし  {sh}/{ss} = "
          f"{sh / ss * 100 if ss else 0:.1f}%")
    print(f"    違う LFRQ どうし  {dh}/{ds} = "
          f"{dh / ds * 100 if ds else 0:.1f}%")
    print("  **前者が後者を上回らなければ、値列は LFRQ では決まっていない。**")


def lfo_period(lfrq):
    """§2.1 の規則で決まる LFO の更新周期 [samples]。上位ニブルだけで決まる。"""
    return 2 ** (18 - (lfrq >> 4))


# ---- W≠3 の段の間隔を測る（--stair） ------------------------------------

def stair_period(olp, path, mode, grid):
    """階段状の LFO 波形（W=0 鋸 / W=2 三角）の段の間隔 [samples] を返す。

    返すのは (段の間隔, 境界の数, 段差の中央値)。

    AM の対数振幅も PM の cos(ω) も LFO 値の単調関数なので、鋸・三角では
    **階段状のランプ**になる。段の境界はブロック値の 1 階差分に立つので、
    その間隔を数えれば段の間隔が直接出る。W=3（ノイズ）と違って値が単調に
    動くため、`--runs` の「同じ語を続けて引いた回数」という測り方は使えない。

    `grid` は段より細かいブロック長。**間隔の分解能は ±grid** になる。
    1 サンプルずつ見ないのは、1 段ぶんの段差（対数振幅で 0.01 前後）が
    包絡の雑音より小さく、ブロック内で平均しないと境界が立たないため。

    **grid の選び方は結論を縛らない。**`grid` は測れる間隔の刻みを決めるだけで、
    どちらのモデルの値も同じ刻みの上に載る（既定では実機モデルの予測が
    `STAIR_SUB` ブロック、ymfm モデルの予測はその 16/31〜1 倍に当たる）。
    """
    d, edges, _, _ = stair_edges(olp, path, mode, grid)

    gaps = sorted(b - a2 for a2, b in zip(edges, edges[1:]))
    med = gaps[len(gaps) // 2]
    if med < STAIR_GAP_MIN:
        raise ValueError(f"格子が粗すぎる: 1 段 {med} ブロック")
    # 鋸の折り返しは段差が桁違いに大きい。間隔の統計には中央値を使うので
    # 折り返しが混ざっても効かないが、段差の方は上位 10% を落としておく
    hs = sorted(abs(d[i]) for i in edges)
    hs = hs[:max(1, int(len(hs) * 0.9))]
    return med * grid, len(edges), hs[len(hs) // 2]


def stair_grid(lfrq):
    """--stair / --cycle の既定のブロック長。§2.1 の規則値を `STAIR_SUB` で割る。"""
    return max(STAIR_GRID_MIN, lfo_period(lfrq) // STAIR_SUB)


def stair_edges(olp, path, mode, grid):
    """段の境界（ブロック番号）と、そのブロック間差分を返す。

    `stair_period` が中でやっていることを、`--cycle` からも使えるように
    切り出したもの。返すのは (差分列, 境界のブロック番号, 段差の中央値,
    段が増える向きか)。
    """
    x, _ = olp.load_left(path)
    a = olp.Analyzer(x, mode)
    v = block_values(a, mode, grid, 0)
    if len(v) < 4 * STAIR_SUB:
        raise ValueError(f"ブロックが少なすぎる: {len(v)}")

    d = [v[i + 1] - v[i] for i in range(len(v) - 1)]
    mag = sorted(abs(y) for y in d)
    thr = STAIR_THR * mag[int(len(mag) * 0.90)]
    if thr <= 0.0:
        raise ValueError("段差が立たない")

    edges = []
    for i, y in enumerate(d):
        if abs(y) >= thr and not (edges and i - edges[-1] <= 1):
            edges.append(i)
    if len(edges) < STAIR_EDGES_MIN:
        raise ValueError(f"境界が少なすぎる: {len(edges)}")

    hs = sorted(abs(d[i]) for i in edges)
    up = sum(1 for i in edges if d[i] > 0) > len(edges) // 2
    return d, edges, hs[len(hs) // 2], up


def stair_cycle(olp, path, mode, grid, period):
    """鋸 (W=0) の 1 周期を折り返しで切り出し、そのあいだの段数を数える。

    返すのは (1 周期の長さ [samples], 折り返しの位置, 1 周期の段数,
    2 段ぶんの段差の割合)。

    **これが「歩幅」の測定そのもの。**`--stair` は段の**間隔**しか測らないので、
    「下位ニブルは間隔ではなく歩幅を変える」の後半を数値にできない。段差の
    中央値は `+1` 段ぶんと `+2` 段ぶんが混ざるので当てにならない。そこで
    **1 周期あたりの段数**を数える。LFO 値が 1 周期で 256 進む以上、
    1 周期の段数 `N` が分かれば平均の歩幅は `256 / N` になる。

    折り返しは鋸が全振幅ぶん戻る 1 点なので、段差が 1 段ぶんの
    `CYCLE_WRAP_RATIO` 倍を超え、かつ普段と逆符号の点として拾える。
    滲みは `CYCLE_WRAP_MERGE` ブロック以内をまとめて 1 本にする。
    キャプチャ頭の `CYCLE_SKIP` ブロックは KEY ON の立ち上がりが折り返しに
    化けるので捨てる。
    """
    d, edges, hmed, up = stair_edges(olp, path, mode, grid)

    rev = [(-y if up else y) for y in d]
    wraps = []
    for i in range(CYCLE_SKIP, len(rev)):
        if rev[i] <= CYCLE_WRAP_RATIO * hmed:
            continue
        if wraps and i - wraps[-1] <= CYCLE_WRAP_MERGE:
            if rev[i] > rev[wraps[-1]]:
                wraps[-1] = i
        else:
            wraps.append(i)
    if len(wraps) < 2:
        raise ValueError(f"折り返しが {len(wraps)} 本しか無い")

    gaps = sorted((wraps[j + 1] - wraps[j]) * grid
                  for j in range(len(wraps) - 1))
    cyc = gaps[len(gaps) // 2]
    nsteps = cyc / period

    # 段差を 1 段ぶん / 2 段ぶんに分ける。1 段ぶんの大きさは「段差の平均が
    # 平均の歩幅（256/N 段ぶん）に当たる」ことから逆算する。段差の分布そのものに
    # 閾値を置く（中央値など）と、2 段ぶんが多数派になる下位ニブルで破綻する
    #
    # **1 段が `CYCLE_BIG_MIN` ブロックを切ると出さない。**段差そのものが
    # ブロックの平均に丸められてしまい、`+1` と `+2` の区別が付かなくなる。
    # 段の間隔や 1 周期の長さは折り返しの位置だけで決まるのでこの制約を受けない
    inner = [abs(d[i]) for i in edges
             if (d[i] > 0) == up and abs(d[i]) < CYCLE_WRAP_RATIO * hmed]
    big = None
    if inner and nsteps > 0 and period // grid >= CYCLE_BIG_MIN:
        unit = sum(inner) / len(inner) * nsteps / 256.0
        big = sum(1 for h in inner if h > CYCLE_BIG_RATIO * unit) / len(inner)
    return cyc, wraps, nsteps, big


def report_cycle(paths, mode, grid=None):
    """[R] 鋸 (W=0) の 1 周期の段数を数え、歩幅を数値にする。"""
    olp = load_analyzer()
    section(f"[R] 鋸 (W=0) の 1 周期あたりの段数と歩幅（--mode {mode}）")
    print("折り返しで 1 周期を切り出し、そのあいだの段数を数える。")
    print("  1 周期で LFO 値は 256 進むので、平均の歩幅 = 256 / 1 周期の段数。")
    print("  デコード表の予測: 1 周期の段数 = 4096/(16+lo) / 歩幅 = (16+lo)/16")
    print("  **上位ニブルには依らない。**")
    print()
    print("  LFRQ  格子  1周期[samples]      予測      比   段数   予測   "
          "歩幅   予測   2段ぶんの段差  予測 lo/16")

    dev = []
    for path in sorted(paths):
        m = NAME_RE.search(Path(path).name)
        if not m:
            print(f"* {Path(path).name}: ファイル名から条件を読めない")
            continue
        lfrq = int(m[2], 16)
        hi, lo = lfrq >> 4, lfrq & 0x0F
        g = grid or stair_grid(lfrq)
        period = lfo_period(lfrq)
        want_cyc = 2 ** 30 / ((16 + lo) * 2 ** hi)
        want_n = 4096 / (16 + lo)
        try:
            cyc, wraps, nsteps, big = stair_cycle(olp, path, mode, g, period)
        except Exception as e:                        # noqa: BLE001
            print(f"  {lfrq:02x}  {g:4d}   {e}")
            continue
        # 折り返しの位置はブロック単位でしか決まらないので、2 本ぶんで
        # ±2 grid サンプル = ±2 grid / period 段のぶれが乗る
        dev.append((nsteps - want_n, 2.0 * g / period))
        bs = f"{big * 100:11.0f}%" if big is not None else "          -"
        print(f"  {lfrq:02x}  {g:4d}  {cyc:12d} {want_cyc:9.0f}  {cyc / want_cyc:6.3f} "
              f"{nsteps:6.1f} {want_n:6.1f} {256 / nsteps:6.3f} "
              f"{(16 + lo) / 16:6.3f} {bs}  {lo / 16 * 100:7.0f}%")

    if dev:
        print()
        print(f"  計 {len(dev)} 条件 / 段数の予測とのずれ 最大 "
              f"{max(abs(x) for x, _ in dev):.2f} 段 "
              f"（折り返しの位置の分解能から来るぶれは最大 "
              f"±{max(r for _, r in dev):.2f} 段）")


def report_stair(paths, mode, grid=None):
    """[N] W≠3 の段の間隔を測り、2 つのモデルの予測と突き合わせる。"""
    olp = load_analyzer()
    section(f"[N] W≠3 の段の間隔（--mode {mode}）")
    print("鋸・三角では LFO 値が単調に動くので、階段の段差から段の間隔を直接測れる。")
    print("  実機モデル  間隔 = 2^(18-hi)           下位ニブルに依らない")
    print("  ymfm モデル 間隔 = 2^22/((16+lo)<<hi)  下位ニブルで 16/31 倍まで縮む")
    print("  **下位ニブル 0 は両モデルが同値なので判別に使えない。**")
    print()
    print("  LFRQ   格子   実測 間隔    実機   ymfm    段差  境界  判定")

    tally = Counter()
    for path in sorted(paths):
        m = NAME_RE.search(Path(path).name)
        if not m:
            print(f"* {Path(path).name}: ファイル名から条件を読めない")
            continue
        lfrq = int(m[2], 16)
        g = grid or stair_grid(lfrq)
        want_hw, want_ym = lfo_period(lfrq), model_period(lfrq)
        try:
            meas, nedge, h = stair_period(olp, path, mode, g)
        except Exception as e:                        # noqa: BLE001
            print(f"  {lfrq:02x}   {g:5d}   {e}")
            tally["測れない"] += 1
            continue
        if abs(want_hw - want_ym) / want_hw < AGREE_TOL:
            verdict = "（両モデル同値）"
        elif abs(meas - want_hw) / want_hw <= AGREE_TOL * 5:
            verdict = "実機"
        elif abs(meas - want_ym) / want_ym <= AGREE_TOL * 5:
            verdict = "ymfm"
        else:
            verdict = "どちらでもない"
        tally[verdict] += 1
        print(f"  {lfrq:02x}   {g:5d}  {meas:10d} {want_hw:7d} {want_ym:6.1f} "
              f"{h:7.3f} {nedge:5d}  {verdict}")

    n = sum(tally.values())
    dec = n - tally["（両モデル同値）"]
    print()
    print(f"  計 {n} 条件 / 判別できる（下位ニブル≠0）{dec} 条件")
    for k in ("実機", "ymfm", "どちらでもない", "測れない"):
        if tally[k]:
            print(f"    {k}: {tally[k]}")


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
                elif want > 0.9 and abs(lag - lag_want) > 1:
                    # ±1 は許す。格子の位相合わせ（`bits_series`）が採る位相が
                    # 2 本で違うと、ブロックの番号づけが 1 段ぶんずれうる
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


# --bits の一致率に許す絶対誤差。2 値化しているので標本ゆらぎは
# sqrt(0.25/600) = 2% 程度
BITS_TEST_TOL = 0.06


def self_test_bits(olp, tg, tmp):
    """既知の値列を入れた人工信号で `--bits` を検証する（実機不要）。

    `--values` の自己検証と同じ人工信号を使うが、見るのは 2 値化した列の
    ハミング距離。**要点は「遅れが VALUE_LAG より大きくても見つかること」**で、
    これが実機データで中間の下位ニブルを判定できなかった原因そのもの
    （README §5.6.1 / §5.9）。
    """
    period = 256
    # 遅れ 300 段（VALUE_LAG=256 より大きい）を試せる長さにする
    nseg = 1200
    length = period * nseg
    nval = nseg + 2
    base = tg.lfo_values_iid(nval, 5200)
    other = tg.lfo_values_iid(nval, 5201)
    half = [other[i] if i % 2 else base[i] for i in range(nval)]
    shift = 300

    # (名前, B の値列, 期待する遅れ, 期待する一致率)。一致率 None は
    # 「BITS_MATCH を超えず、偶然の上限のあたりに収まること」だけを見るケース
    cases = (
        ("同じ列", base, 0, 1.0),
        (f"{shift} 段ずらした列（VALUE_LAG={VALUE_LAG} を超える遅れ）",
         base[shift:] + base[:shift], shift, 1.0),
        ("1 段おきに差し替えた列", half, 0, 0.75),
        ("無関係な列", other, None, None),
    )

    results = []
    for mode in MODES:
        pa = Path(tmp) / f"{mode}_bits_a.wav"
        if not pa.exists():
            tg.write_wav(pa, tg.synth(mode, period, length, tg.CARRIERS[2],
                                      0, values=base))
        _, _, va = bits_series(olp, pa, mode, period)
        pka = pack_bits(series_bits(va))
        for note, vals, lag_want, want in cases:
            pb = Path(tmp) / f"{mode}_bits_{lag_want}_{want}.wav"
            if not pb.exists():
                tg.write_wav(pb, tg.synth(mode, period, length, tg.CARRIERS[2],
                                          0, values=vals))
            name = f"{mode} 2 値化した値列（{note}）"
            err = None
            try:
                _, _, vb = bits_series(olp, pb, mode, period)
                lag, r, n, luck = bit_match(pka, pack_bits(series_bits(vb)))
                if lag is None:
                    err = "突き合わせできない"
                elif want is None:
                    # 無関係な列。遅れを数千通り試した最良値には選択バイアスが
                    # 乗るので 50% ちょうどにはならない。**閾値を超えないこと**と、
                    # 偶然の上限のあたりに収まっていることだけを見る
                    if r >= BITS_MATCH:
                        err = f"無関係な列が閾値を超えた: {r * 100:.1f}%"
                    elif luck is None or r > luck + BITS_TEST_TOL:
                        err = f"一致率 {r * 100:.1f}% が偶然の上限 {luck} から離れた"
                elif abs(r - want) > BITS_TEST_TOL:
                    err = f"一致率 {r * 100:.1f}% (期待 {want * 100:.0f}%)"
                elif want > 0.9 and abs(lag - lag_want) > 1:
                    # ±1 は許す。格子の位相合わせ（`bits_series`）が採る位相が
                    # 2 本で違うと、ブロックの番号づけが 1 段ぶんずれうる
                    err = f"遅れ {lag} (期待 {lag_want})"
                elif want > 0.9 and (luck is None or luck > 0.7):
                    err = f"偶然の上限が高すぎる: {luck}"
            except Exception as e:                    # noqa: BLE001
                err = str(e)
            results.append((name, err))

    # デコード表の予測は純関数なので、実機も人工信号も要らずに検算できる
    want_frac = {lo: min(lo, 16 - lo) / 16 for lo in range(16)}
    bad = [lo for lo in range(16) if pair_fraction(lo) != want_frac[lo]]
    results.append(("デコード表の予測 min(lo,16-lo)/16",
                    None if not bad else f"外れた lo: {bad}"))
    want_par = ["-"] + ["奇数"] * 7 + ["偶数"] * 8
    bad = [lo for lo in range(16) if pair_parity(lo) != want_par[lo]]
    results.append(("デコード表の予測 パリティが 7 と 8 の間で反転",
                    None if not bad else f"外れた lo: {bad}"))
    return results


# --stair の段の間隔に許す相対誤差。分解能は ±grid = ±段/STAIR_SUB なので
# 1 刻みぶんの余裕を見る
STAIR_TEST_TOL = 1.5 / STAIR_SUB

# --cycle の 1 周期の段数に許す相対誤差。折り返しの位置がブロック単位でしか
# 決まらないぶんのぶれを見込む
CYCLE_TEST_TOL = 0.04


def self_test_cycle(olp, tg, tmp):
    """既知の歩幅の鋸を入れた人工信号で `--cycle` を検証する（実機不要）。

    `synth(..., values=)` に 256 で折り返すランプを渡せば鋸そのものになる。
    **歩幅を変えると 1 周期の段数がその逆数で動く**のが測りたい性質なので、
    歩幅 1 / 2 / 1.5（1 と 2 が交互）の 3 通りで段数が戻るかを見る。
    """
    period, grid = 256, 32
    cases = (
        ((1,), 256.0, "歩幅 1（下位ニブル 0 相当）"),
        ((2,), 128.0, "歩幅 2（下位ニブル f に近い）"),
        ((1, 2), 256.0 / 1.5, "歩幅 1 と 2 が交互（下位ニブル 8 相当）"),
    )

    results = []
    for walk, want, note in cases:
        # 折り返しが 2 本以上入る長さにする
        nval = int(want * 3) + 8
        values, val = [], 0
        for i in range(nval):
            values.append(val % 256)
            val += walk[i % len(walk)]
        length = period * (nval - 2)
        for mode in MODES:
            path = Path(tmp) / f"{mode}_cycle_{'_'.join(map(str, walk))}.wav"
            if not path.exists():
                tg.write_wav(path, tg.synth(mode, period, length,
                                            tg.CARRIERS[1], 0, values=values))
            name = f"{mode} 1 周期の段数 {want:.1f}（{note}）"
            err = None
            try:
                cyc, wraps, n, big = stair_cycle(olp, path, mode, grid, period)
                if abs(n - want) / want > CYCLE_TEST_TOL:
                    err = (f"段数 {n:.1f} (期待 {want:.1f}) / "
                           f"折り返し {len(wraps)} 本")
            except Exception as e:                    # noqa: BLE001
                err = str(e)
            results.append((name, err))
    return results


def self_test_stair(olp, tg, tmp):
    """階段状の値列を入れた人工信号で `--stair` を検証する（実機不要）。

    `synth(..., values=)` に単調なランプを渡せば鋸波そのものになる。要点は
    **歩幅（1 段で値がいくつ進むか）を変えても間隔が変わらずに戻ること**で、
    これが実機モデルと ymfm モデルを判別する性質そのものになる。歩幅 1 と 2 が
    交互になる列（実機の下位ニブル 8 相当）も入れる。ここで間隔が 2 倍に出ると
    閾値 `STAIR_THR` が高すぎることになり、実機データでも同じ誤りが出る。
    """
    cases = (
        (256, 1, "歩幅 1（下位ニブル 0 相当）"),
        (256, 2, "歩幅 2（下位ニブル f 相当）"),
        (256, 3, "歩幅 3（歩幅を変えても間隔は動かない）"),
        (256, (1, 2), "歩幅 1 と 2 が交互（下位ニブル 8 相当）"),
        (1024, 2, "段 1024 / 歩幅 2"),
    )

    results = []
    for period, step, note in cases:
        nval = 600
        walk, val = (step if isinstance(step, tuple) else (step,)), 0
        values = []
        for i in range(nval):
            values.append(val % 256)
            val += walk[i % len(walk)]
        length = period * (nval - 2)
        for mode in MODES:
            tag = f"{mode}_stair_{period}_{step}".replace(" ", "")
            path = Path(tmp) / f"{tag}.wav"
            if not path.exists():
                tg.write_wav(path, tg.synth(mode, period, length,
                                            tg.CARRIERS[1], 0, values=values))
            name = f"{mode} 段の間隔 {period}（{note}）"
            err = None
            try:
                grid = max(STAIR_GRID_MIN, period // STAIR_SUB)
                meas, _, _ = stair_period(olp, path, mode, grid)
                if abs(meas - period) / period > STAIR_TEST_TOL:
                    err = f"間隔 {meas} (期待 {period})"
            except Exception as e:                    # noqa: BLE001
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
        results += self_test_bits(olp, tg, tmp)
        results += self_test_stair(olp, tg, tmp)
        results += self_test_cycle(olp, tg, tmp)

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
    parser.add_argument("--bits", type=Path, nargs="+", metavar="WAV",
                        help="[P] 段ごとの値列を 2 値化し、遅れを全域で探して"
                             "総当たりで突き合わせる（--mode が要る）")
    parser.add_argument("--bits-lag", type=int, default=BITS_LAG, metavar="N",
                        help=f"--bits で探す遅れの幅 [段]（既定 {BITS_LAG}）")
    parser.add_argument("--stair", type=Path, nargs="+", metavar="WAV",
                        help="[N] W≠3（鋸・三角）の段の間隔を測り、2 つのモデルの"
                             "予測と突き合わせる（--mode が要る）")
    parser.add_argument("--cycle", type=Path, nargs="+", metavar="WAV",
                        help="[R] 鋸 (W=0) の 1 周期の段数を数えて歩幅を出す"
                             "（--mode が要る）")
    parser.add_argument("--grid", type=int, default=None, metavar="N",
                        help="--stair のブロック長 [samples]。省略すると "
                             f"2^(18-hi)/{STAIR_SUB} を使う")
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

    if args.bits:
        if not args.mode:
            parser.error("--bits には --mode が要る")
        report_bits(args.bits, args.mode, args.period, args.bits_lag)
        return 0

    if args.stair:
        if not args.mode:
            parser.error("--stair には --mode が要る")
        report_stair(args.stair, args.mode, args.grid)
        return 0

    if args.cycle:
        if not args.mode:
            parser.error("--cycle には --mode が要る")
        report_cycle(args.cycle, args.mode, args.grid)
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
