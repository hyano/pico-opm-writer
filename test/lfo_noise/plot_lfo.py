#!/usr/bin/env python3
"""実測キャプチャから LFO の出力波形（段）を SVG に描く。

[README.md](README.md) §2.1 / §2.2 に貼ってある 3 枚の図を生成する。**主張そのものは
`analyze_lfo.py` の集計が根拠**で、このスクリプトはその根拠を目で見える形に直すだけ。
段の値も格子の位相も `analyze_lfo.py` の関数をそのまま呼んで出しているので、集計と
同じ数字を描いている。

    fig/fig1_lfrq_low_nibble.svg    下位ニブルを振っても段の幅が変わらない
    fig/fig2_lfrq_high_nibble.svg   上位ニブルが 1 増えると段の幅が半分になる
    fig/fig3_nfrq00_f_band.svg      NFRQ=0x00 では f 帯の段が 2 つずつ対になる

```bash
./plot_lfo.py                 # fig/*.svg を 3 枚生成し、検証用の数値を表示
./plot_lfo.py --start 24576   # 切り出しを探し始める位置（サンプル）を変える
```

窓に入った段数が予測とずれていれば `NG` を出して終了コード 1。

**図の中の文字だけは英語**。実測データの図は日本語を読めない相手にもそのまま渡るため。

パネルに何を描いてあるか / 3 枚で何を揃えてあるか / 縦軸の取り方 / 段の境界に
`align_values()` ではなく `grid_edge()` を使う理由 / パネルごとの波形の出どころは
**README.md §4.5「図の生成」** にまとめてある。
"""

import argparse
import importlib.machinery
import importlib.util
import math
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
FIG_DIR = SCRIPT_DIR / "fig"

# 図をまたいで固定する寸法
SPAN = 256                  # 横軸に載せるサンプル数
PX_PER_SAMPLE = 3           # 1 サンプルの幅 [px]
PLOT_W = SPAN * PX_PER_SAMPLE
PLOT_H = 84                 # パネルの描画領域の高さ [px]
PANEL_PITCH = 116           # パネルの縦の間隔 [px]
MARGIN_L = 132
MARGIN_R = 44
MARGIN_T = 96
MARGIN_B = 52
FIG_W = MARGIN_L + PLOT_W + MARGIN_R

# 縦軸（搬送波レベル）のレンジ [dB]。0dB が LFO 値 0（最大振幅）
DB_TOP = 2.0
DB_BOT = -50.0
DB_TICKS = (0.0, -25.0, -50.0)
# 対数振幅（neper）から dB への換算
NEPER_TO_DB = 20.0 / math.log(10.0)
# 0dB 基準に取る分位（段のレベル / 生波形の振幅）
REF_Q = 0.998
AMP_Q = 0.999
# 基準を取るときに捨てる先頭の段数（KON 直後の過渡を避ける）
REF_SKIP = 64
# 切り出しの既定位置 [samples] と、そこから見やすい窓を探す範囲・刻み
START_DEFAULT = 16384
WINDOW_SEARCH = 8192
WINDOW_STEP = 128
# 最小格子（LFO の最短ラッチ間隔）[samples]
BASE_GRID = 8
# 段の境界を探すときに前後で見比べるサンプル数
EDGE_WIDTH = 8
# 「隣の段とレベルがほぼ同じ」と見なす段差 [dB]。同じ語を続けて引いた段の差は
# 実測で 0.21dB に集まり、別の語どうしの差（中央値 14dB）と桁が違う
SAME_DB = 0.5

C_INK = "#1a1a1a"
C_SUB = "#666666"
C_GRID = "#e2e2e2"
C_BAND = "#eef3f9"
C_EDGE = "#9aa4ad"
C_WAVE = "#7f8f9f"
C_LINE = "#1b6ac9"
C_MARK = "#d94a2b"
FONT = "-apple-system,'Helvetica Neue',Helvetica,Arial,sans-serif"


def load_module(name, path):
    """スクリプトをモジュールとして読む。"""
    loader = importlib.machinery.SourceFileLoader(name, str(path))
    spec = importlib.util.spec_from_loader(name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


AZ = load_module("analyze_lfo", SCRIPT_DIR / "analyze_lfo.py")
OLP = load_module("opm_lfo_period", AZ.ANALYZER)


def grid_edge(x, period, width=EDGE_WIDTH):
    """段の境界が来るサンプル位置（`period` を法とする）を生波形から決める。

    **`analyze_lfo.align_values()` はここでは使えない。** あちらは「変化した段の
    割合 `c` が最小になる位相」を正しい整列と見なすが、その判定が効くのは同じ語を
    続けて引く回がある帯（`P_lfo <= P_n`）だけ。更新のたびに必ず新しい語が来る帯
    （上位ニブル `e` 以下）では、位相がずれたブロックは隣り合う 2 語の混合になって
    **互いに似てしまい `c` が下がる**ので、最小値がずれた位相を指してしまう。
    実際 `LFRQ=0xb0` では段の途中を境界と判定する（実測: 正しい境界は 5、
    `align_values()` の答えは 61）。

    ここでは段の境界そのものを直接測る。境界では搬送波のレベルが跳ぶので、
    各サンプル位置 `t` について前後 `width` サンプルの対数エネルギーの差を取り、
    `period` を法とする位置ごとに積み上げて最大の位置を選ぶ。ヒストグラムは
    1 つの剰余に鋭く立つ（`LFRQ=0xd0` で上位 2000 点のうち 514 点が同じ剰余）。
    """
    n = len(x)
    w = min(period // 2, width)
    acc = [0.0] * (n + 1)
    for i, v in enumerate(x):
        acc[i + 1] = acc[i] + float(v) * v

    def energy(a, b):
        e = (acc[b] - acc[a]) / (b - a)
        return 0.5 * math.log(e) if e > 0.0 else -20.0

    best = (None, 0)
    for o in range(period):
        total = 0.0
        for t in range(o + period, n - w, period):
            if t >= w:
                total += abs(energy(t, t + w) - energy(t - w, t))
        if best[0] is None or total > best[0]:
            best = (total, o)
    return best[1]


class Capture:
    """1 キャプチャぶんの生波形と段ごとのレベル。

    段の境界は `grid_edge()` で生波形から決め、`block_values()` にはそこから
    `BLOCK_GUARD` を引いた位相を渡す。`block_values()` のブロック `k` は
    `[off + kP + guard, off + (k+1)P)` なので、これで **ブロックが段
    `[edge + kP, edge + (k+1)P)` の中にちょうど収まる**。

    `changed_fraction()` は既定で 256 段を要求する。`P` が大きいキャプチャは
    それだけの段が取れないので、取れる段数に合わせて要求を下げる。
    """

    def __init__(self, name, lfrq):
        self.name = name
        self.x, _ = OLP.load_left(SCRIPT_DIR / name)
        a = OLP.Analyzer(self.x, "am")
        self.period = AZ.lfo_period(lfrq)
        self.edge = grid_edge(self.x, self.period)
        v = AZ.block_values(a, "am", self.period,
                            (self.edge - AZ.BLOCK_GUARD) % self.period)
        nb = len(v)
        self.c, _t = AZ.changed_fraction(v, min_blocks=min(AZ.BLOCKS_MIN,
                                                           nb - 4))
        tail = sorted(v[REF_SKIP:])
        ref = tail[min(len(tail) - 1, int(len(tail) * REF_Q))]
        self.level = [(q - ref) * NEPER_TO_DB for q in v]
        amp = sorted(abs(s) for s in self.x[REF_SKIP * self.period:])
        self.ref_amp = max(1, amp[min(len(amp) - 1, int(len(amp) * AMP_Q))])

    def window(self, start):
        """`start` 以降で最初に来る段の境界から SPAN サンプルぶんを切り出す。

        返すのは (段ごとのレベル [dB], 生サンプル)。
        """
        count = SPAN // self.period
        first = max(0, -(-(start - self.edge) // self.period))
        first = min(first, len(self.level) - count)
        head = self.edge + first * self.period
        return self.level[first:first + count], self.x[head:head + SPAN]

    def same_rate(self):
        """隣の段とレベルがほぼ同じだった境界の割合（キャプチャ全体）。"""
        v = self.level
        return sum(1 for a, b in zip(v, v[1:])
                   if abs(b - a) < SAME_DB) / max(1, len(v) - 1)


def pick_start(caps, start, metric, span=WINDOW_SEARCH, step=WINDOW_STEP):
    """図の全パネルが見やすくなる切り出し位置を選ぶ。

    窓の位置は結論に関わらない（どの図も主張は段の幅とその比であって、どの区間の
    ノイズ語を引いたかではない）が、たまたま似たレベルが続く区間を引くとパネルが
    平坦に見えてしまう。`start` から `span` サンプルの範囲を `step` 刻みで試し、
    **最も見えにくいパネル**の指標が最大になる位置を選ぶ（1 枚でも平坦なら落とす）。

    指標は 2 通り。段の幅を見せる図（図 1 / 図 2）では `"changes"` を使い、段差が
    見える境界の割合を最大にする（レベルが変わらない更新が続くと境界が絵に現れず、
    段が広くなったように見えるため）。**レベルが変わらない境界そのものが主題の
    図 3 ではこれを使えない**ので、`"range"`（段の振れ幅）で選ぶ。こちらは同じ語を
    続けて引いたかどうかとは無関係な指標。

    主指標は粗く量子化してから比べる。同点になった候補のなかから、**どのパネルにも
    レベルの高い段が入る**位置（重ねた生波形が細い線に潰れない位置）を採る。
    """
    def score(cand):
        wins = [c.window(cand)[0] for c in caps]
        spread = min(max(w) - min(w) for w in wins)
        loud = min(max(w) for w in wins)
        if metric == "range":
            return (round(spread / 2.0), loud)
        changed = min(sum(1 for a, b in zip(w, w[1:]) if abs(b - a) >= SAME_DB)
                      / max(1, len(w) - 1) for w in wins)
        return (round(changed / 0.05), loud, spread)

    return max(range(start, start + span, step), key=score)


def esc(s):
    return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def text(x, y, s, size=13, fill=C_INK, anchor="start", weight="normal"):
    return (f'<text x="{x:.1f}" y="{y:.1f}" font-family="{FONT}" '
            f'font-size="{size}" fill="{fill}" text-anchor="{anchor}" '
            f'font-weight="{weight}">{esc(s)}</text>')


def draw_panel(top, label, cap, steps, raw, mark_same):
    """パネル 1 枚ぶんの SVG 断片。`label` は改行で複数行に割る。"""
    out = []
    bot, period = top + PLOT_H, cap.period
    mid = top + PLOT_H / 2

    def yv(db):
        lv = max(DB_BOT, min(DB_TOP, db))
        return top + (DB_TOP - lv) / (DB_TOP - DB_BOT) * PLOT_H

    for k in range(0, SPAN // period, 2):    # 段を 1 つおきに塗る
        x = MARGIN_L + k * period * PX_PER_SAMPLE
        out.append(f'<rect x="{x:.1f}" y="{top}" '
                   f'width="{period * PX_PER_SAMPLE}" height="{PLOT_H}" '
                   f'fill="{C_BAND}"/>')
    for db in DB_TICKS:                      # 横のガイド
        y = yv(db)
        out.append(f'<line x1="{MARGIN_L}" y1="{y:.1f}" '
                   f'x2="{MARGIN_L + PLOT_W}" y2="{y:.1f}" '
                   f'stroke="{C_GRID}" stroke-width="1"/>')
    for i in range(0, SPAN + 1, BASE_GRID):  # 最小格子（8 サンプル）
        x = MARGIN_L + i * PX_PER_SAMPLE
        out.append(f'<line x1="{x:.1f}" y1="{top}" x2="{x:.1f}" y2="{bot}" '
                   f'stroke="{C_GRID}" stroke-width="1"/>')
    for i in range(0, SPAN + 1, period):     # 段の境界
        x = MARGIN_L + i * PX_PER_SAMPLE
        out.append(f'<line x1="{x:.1f}" y1="{top}" x2="{x:.1f}" y2="{bot}" '
                   f'stroke="{C_EDGE}" stroke-width="1"/>')

    half = PLOT_H / 2 - 1                    # 生波形（線形の振幅）
    pts = []
    for i, s in enumerate(raw):
        y = mid - max(-1.0, min(1.0, s / cap.ref_amp)) * half
        pts.append(f"{MARGIN_L + i * PX_PER_SAMPLE:.1f},{y:.1f}")
    out.append(f'<polyline points="{" ".join(pts)}" fill="none" '
               f'stroke="{C_WAVE}" stroke-width="1" opacity="0.85"/>')

    pts = []                                 # 段ごとのレベル
    for k, db in enumerate(steps):
        y = yv(db)
        pts.append(f"{MARGIN_L + k * period * PX_PER_SAMPLE:.1f},{y:.1f}")
        pts.append(f"{MARGIN_L + (k + 1) * period * PX_PER_SAMPLE:.1f},{y:.1f}")
    out.append(f'<polyline points="{" ".join(pts)}" fill="none" '
               f'stroke="{C_LINE}" stroke-width="2.2" stroke-linejoin="miter"/>')
    out.append(f'<rect x="{MARGIN_L}" y="{top}" width="{PLOT_W}" '
               f'height="{PLOT_H}" fill="none" stroke="{C_EDGE}" '
               f'stroke-width="1"/>')

    if mark_same:                            # レベルが変わらなかった境界
        for k in range(1, len(steps)):
            if abs(steps[k] - steps[k - 1]) < SAME_DB:
                x = MARGIN_L + k * period * PX_PER_SAMPLE
                out.append(f'<path d="M{x - 4.5:.1f},{bot + 4} '
                           f'L{x + 4.5:.1f},{bot + 4} L{x:.1f},{bot + 12} Z" '
                           f'fill="{C_MARK}"/>')

    lines = label.split("\n")
    for j, line in enumerate(lines):
        out.append(text(MARGIN_L - 14, top + 19 + j * 18, line, 14,
                        C_INK, "end", "bold"))
    out.append(text(MARGIN_L - 14, top + 21 + len(lines) * 18,
                    f"P = {period} samples", 12, C_SUB, "end"))
    for db in DB_TICKS:                      # 縦軸の目盛
        out.append(text(MARGIN_L + PLOT_W + 6, yv(db) + 4, f"{db:.0f}",
                        10, C_SUB))
    return "\n".join(out)


def build(title, notes, panels, metric, mark_same, start):
    """図 1 枚を組み立てる。`panels` は (ラベル, ファイル名, LFRQ) の並び。"""
    caps = [Capture(name, lfrq) for _label, name, lfrq in panels]
    start = pick_start(caps, start, metric)

    height = MARGIN_T + PANEL_PITCH * (len(panels) - 1) + PLOT_H + MARGIN_B
    body = [text(16, 26, title, 15, C_INK, "start", "bold")]
    body += [text(16, 46 + 16 * i, n, 12, C_SUB) for i, n in enumerate(notes)]
    report = []
    for idx, ((label, _name, _lfrq), cap) in enumerate(zip(panels, caps)):
        steps, raw = cap.window(start)
        body.append(draw_panel(MARGIN_T + PANEL_PITCH * idx, label, cap,
                               steps, raw, mark_same))
        report.append((label, cap.period, len(steps), SPAN // cap.period,
                       cap.c, cap.same_rate()))

    axis_y = MARGIN_T + PANEL_PITCH * (len(panels) - 1) + PLOT_H
    for i in range(0, SPAN + 1, 32):         # 横軸
        x = MARGIN_L + i * PX_PER_SAMPLE
        body.append(f'<line x1="{x:.1f}" y1="{axis_y + 16}" x2="{x:.1f}" '
                    f'y2="{axis_y + 21}" stroke="{C_SUB}" stroke-width="1"/>')
        body.append(text(x, axis_y + 34, str(i), 11, C_SUB, "middle"))
    body.append(text(MARGIN_L + PLOT_W, axis_y + 52,
                     "sample (fs = 62500 Hz)", 12, C_SUB, "end"))

    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{FIG_W}" '
            f'height="{height}" viewBox="0 0 {FIG_W} {height}">\n'
            f'<rect width="{FIG_W}" height="{height}" fill="#ffffff"/>\n'
            + "\n".join(body) + "\n</svg>\n"), report


COMMON = ("Grey: captured samples, linear amplitude.  "
          "Blue: carrier level of each step [dB], 0 dB = LFO value 0.",
          "Shaded band = one step;  thin gridlines = 8 samples.  "
          "All panels and all three figures share the same scales.")

FIG1 = ("Low nibble of LFRQ does not change the step width "
        "(high nibble fixed to d, NFRQ=0x1f)",
        ("All five keep a 32-sample step: exactly 8 steps fit "
         "in the same 256-sample window.",) + COMMON,
        "changes",
        [("LFRQ = 0xd0", "wav/am_nfrq_1f_lfrq_d0_kc_4a_mul_04.wav.zst", 0xD0),
         ("LFRQ = 0xd4", "wav/am_nfrq_1f_lfrq_d4_kc_4a_mul_04.wav.zst", 0xD4),
         ("LFRQ = 0xd8", "wav/am_nfrq_1f_lfrq_d8_kc_52_mul_04.wav.zst", 0xD8),
         ("LFRQ = 0xdc", "wav/am_nfrq_1f_lfrq_dc_kc_52_mul_04.wav.zst", 0xDC),
         ("LFRQ = 0xdf", "wav/am_nfrq_1f_lfrq_df_kc_52_mul_04.wav.zst", 0xDF)])

FIG2 = ("Raising the high nibble of LFRQ by one halves the step width "
        "(low nibble fixed to 0, NFRQ=0x1f)",
        ("Steps per 256-sample window: 2 - 4 - 8 - 16 - 32.",) + COMMON,
        "changes",
        [("LFRQ = 0xb0", "wav/am_nfrq_1f_lfrq_b0_kc_4a_mul_04.wav.zst", 0xB0),
         ("LFRQ = 0xc0", "wav/am_nfrq_1f_lfrq_c0_kc_52_mul_04.wav.zst", 0xC0),
         ("LFRQ = 0xd0", "wav/am_nfrq_1f_lfrq_d0_kc_4a_mul_04.wav.zst", 0xD0),
         ("LFRQ = 0xe0", "wav/am_nfrq_1f_lfrq_e0_kc_52_mul_04.wav.zst", 0xE0),
         ("LFRQ = 0xf0", "wav/am_nfrq_1f_lfrq_f0_kc_52_mul_08.wav.zst", 0xF0)])

FIG3 = ("With NFRQ=0x00 the steps of the f band come in identical pairs",
        ("The step stays 8 samples wide in both f-band panels.  "
         "A red mark is a boundary where the level barely moved (< 0.5 dB):",
         "only the middle panel has one at every other boundary.") + COMMON,
        "range",
        [("LFRQ = 0xf0\nNFRQ = 0x1f",
          "wav/am_nfrq_1f_lfrq_f0_kc_52_mul_08.wav.zst", 0xF0),
         ("LFRQ = 0xf0\nNFRQ = 0x00",
          "wav/am_nfrq_00_lfrq_f0_kc_52_mul_08.wav.zst", 0xF0),
         ("LFRQ = 0xe0\nNFRQ = 0x00",
          "wav/am_nfrq_00_lfrq_e0_kc_52_mul_04.wav.zst", 0xE0)])

JOBS = (("fig1_lfrq_low_nibble.svg", FIG1, False),
        ("fig2_lfrq_high_nibble.svg", FIG2, False),
        ("fig3_nfrq00_f_band.svg", FIG3, True))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--start", type=int, default=START_DEFAULT,
                    help="切り出しを探し始める位置 [samples]")
    ap.add_argument("--out-dir", type=Path, default=FIG_DIR,
                    help="SVG の出力先ディレクトリ")
    args = ap.parse_args()
    args.out_dir.mkdir(exist_ok=True)

    bad = 0
    for fname, (title, notes, metric, panels), mark in JOBS:
        svg, report = build(title, notes, panels, metric, mark, args.start)
        (args.out_dir / fname).write_text(svg, encoding="utf-8")
        print(f"\n{fname}")
        for label, period, got, want, c, same in report:
            ok = "OK" if got == want else "NG"
            bad += got != want
            print(f"  {label.replace(chr(10), ' / '):24s} P={period:4d}"
                  f"  窓内の段数 {got:3d}/{want:3d} {ok}"
                  f"  変化した段 c={c:.3f}"
                  f"  レベルが同じ境界 {same * 100:5.1f}%")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
