#!/usr/bin/env python3
"""
NE で直接出したノイズのキャプチャから、ノイズ発生器の周期と系列を測る。

[README.md](README.md) の結論はすべてこのスクリプトの出力が根拠。

## なぜ LFO 経由と別に要るのか

`test/lfo_noise/` は LFO の値からノイズ語の供給周期 `P_n` を測っているが、LFO の
ラッチ格子が最短 8 サンプルなので `P_n` が 8 サンプルを下回る側（`NFRQ >= 0x10`）は
「毎段が新しい語」に飽和して区別できない。NE (`0x0F` の bit7) でノイズを
チャネル 7 の C2 に直接出すと、**DAC の 1 サンプルが格子になる**ので 8 倍細かく測れる。

## 出力の性質

NE の出力は **EG のレベルにノイズ発生器のビットで符号を付けた 2 値**で、符号は
1 サンプルに 1 回ラッチされる。したがって:

- L チャネルの符号列 = ノイズ発生器のビット列そのもの
- 符号が変わりうるのは、ノイズ発生器が新しい語を出した瞬間だけ
- 隣り合うサンプルで符号が変わる割合 `r` は、1 サンプルあたりの更新回数の半分
  （更新のたびに 1/2 の確率で符号が反転するため）。よって **`P_n` = 0.5 / `r`**

`P_n` が 1 サンプルを下回ると 1 サンプルに複数回更新が入り、`r` は 0.5 で頭打ちになる。
測れるのは `P_n >= 1`、すなわち `NFRQ <= 0x1e` まで。

## 使い方

```bash
./analyze_noise.py                      # wav_ne/ を全部解析して集計を出す
./analyze_noise.py --wav-dir ./wav_ne
./analyze_noise.py --period wav_ne/*.wav.zst    # 系列の周期を探す（時間がかかる）
./analyze_noise.py --self-test          # 人工信号で検証する（実機不要）
```

外部ライブラリは使わない（標準ライブラリのみ）。

SPDX-License-Identifier: MIT
"""

import argparse
import importlib.util
import math
import random
import re
import sys
from collections import Counter
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
ANALYZER = SCRIPT_DIR.parent.parent / "tools" / "opm-lfo-period.py"

# ファイル名から NFRQ を取り出す。NE を含めた値が入っているので bit7 は落とす
NAME_RE = re.compile(r"_nfrq_([0-9a-f]{2})")

# 2 値であることを確かめるとき、この割合以上が上位 2 値に入っていること
TWO_LEVEL_MIN = 0.999
# 連長が「P_n の整数倍」と見なす許容 [samples]。更新の瞬間は P_n おきに来るが
# サンプルは整数位置にしか無いので、P_n が半端な値のとき連長は 1 サンプルまでずれる
RUN_TOL = 1.0
# 系列の周期を探すときに照合するパターンの長さ [steps]
SEQ_WINDOW = 64
# ずらした列との一致率がこれを超えたら「その遅れが周期」と見なす
PERIOD_HIT = 0.98
# 実測で決まった LFSR の段数とタップ（README §2.4）
LFSR_WIDTH = 17
LFSR_TAPS = (14, 17)
# 漸化式の同定に使う先頭のステップ数（段数の 2 倍あれば足りる）
BM_SAMPLE = 512


def load_bits(path):
    """L チャネルの符号列を 0/1 で返す。2 値でなければ例外。

    `tools/opm-lfo-period.py` の `load_left` をそのまま使う（`.wav.zst` を直接読む）。
    """
    spec = importlib.util.spec_from_file_location("opm_lfo_period", ANALYZER)
    olp = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(olp)
    x, _ = olp.load_left(path)
    if len(x) < 1024:
        raise ValueError(f"サンプルが少なすぎる: {len(x)}")
    common = Counter(x).most_common(2)
    if len(common) < 2 or sum(n for _, n in common) < TWO_LEVEL_MIN * len(x):
        raise ValueError("2 値になっていない（NE が効いていない可能性）")
    hi = max(v for v, _ in common)
    return [1 if v >= hi else 0 for v in x], common


def transition_rate(bits):
    """隣り合うサンプルで符号が変わった割合。"""
    return sum(1 for a, b in zip(bits, bits[1:]) if a != b) / (len(bits) - 1)


def run_lengths(bits):
    """同じ符号が続いた長さの列。"""
    out, c = [], 1
    for a, b in zip(bits, bits[1:]):
        if a == b:
            c += 1
        else:
            out.append(c)
            c = 1
    out.append(c)
    return out


def supply_period(bits):
    """符号の変化率から `P_n` [samples] を出す。飽和していれば None も返す。"""
    r = transition_rate(bits)
    if r <= 0.0:
        return None, r
    return 0.5 / r, r


def grid_fraction(runs, period):
    """連長が `period` の整数倍に乗っている割合。

    `P_n` が正しければ、符号が変わりうるのは更新の瞬間だけなので連長は必ず
    `P_n` の整数倍になる。**`P_n` の推定が当たっているかどうかの独立な点検**で、
    変化率からの推定とは別の情報を使う。
    """
    if period is None or period <= 0.0 or not runs:
        return 0.0
    ok = sum(1 for r in runs
             if abs(r - max(1, round(r / period)) * period) <= RUN_TOL)
    return ok / len(runs)


def step_bits(bits, period):
    """ノイズ発生器の**ステップごと**のビット列を復元する。

    符号が変わりうるのは更新の瞬間だけなので、連長を `period` で割れば
    「そのビットが何ステップ続いたか」になる。サンプル単位のままだと同じビットが
    `period` 個ずつ並ぶだけで、系列の周期を探すのに無駄が多い。

    **`period` を掛けて位置を出す（`k * period` の位置を拾う）やり方は使えない。**
    `period` の推定にはわずかな誤差があり、10 万ステップも進むと数十ステップぶん
    ずれる。連長から積み上げれば誤差が溜まらない。
    """
    if period is None or period <= 0.0 or not bits:
        return []
    out, val = [], bits[0]
    for r in run_lengths(bits):
        out.extend([val] * max(1, round(r / period)))
        val ^= 1
    return out


def sequence_period(steps, window=SEQ_WINDOW):
    """ステップ列が何ステップで繰り返すかを返す。見つからなければ (None, 0)。

    先頭 `window` ステップのパターンが次に現れる位置を 1 パスで探し、そこから
    先が全部一致するかを確かめる。遅れの総当たりだと `2^17` ステップ級の周期には
    現実的な時間で届かない（1 遅れごとに全長を舐めることになる）。
    `window` ビットあれば、この長さの系列で偶然一致することはまず無い。
    """
    n = len(steps)
    if n < 4 * window:
        return None, 0.0
    key, mask, cur = 0, (1 << window) - 1, 0
    for b in steps[:window]:
        key = (key << 1) | b
    for i, b in enumerate(steps):
        cur = ((cur << 1) | b) & mask
        lag = i - window + 1
        if lag <= 0 or cur != key:
            continue
        m = n - lag
        same = sum(1 for j in range(m) if steps[j] == steps[j + lag])
        if same >= PERIOD_HIT * m:
            return lag, same / m
    return None, 0.0


# ---- 集計 ---------------------------------------------------------------

def section(title):
    print()
    print(title)
    print("-" * len(title))


def report(wav_dir, want_period):
    files = sorted(wav_dir.glob("*.wav.zst"))
    if not files:
        print(f"! {wav_dir} に .wav.zst が無い", file=sys.stderr)
        return 1

    section(f"[N] NFRQ とノイズ発生器の更新周期  ({wav_dir.name})")
    print("NE でノイズを直接出し、L の符号列から更新周期を測る。")
    print("  予測は P_n = (32 - NFRQ) / 2 [samples]")
    print("  変化率 r = 符号が変わったサンプルの割合。P_n = 0.5 / r")
    print("  整数倍% = 連長が P_n の整数倍に乗っている割合（別経路の点検）")
    print("  P_n < 1 では 1 サンプルに複数回更新が入るので r は 0.5 で頭打ちになる")
    print()
    print("nfrq   予測 P_n   実測 P_n   実測/予測   変化率 r   整数倍%   サンプル")
    ng = 0
    for path in files:
        m = NAME_RE.search(path.name)
        if not m:
            continue
        nfrq = int(m.group(1), 16) & 0x1F
        try:
            bits, _ = load_bits(path)
        except Exception as e:                        # noqa: BLE001
            print(f"  {nfrq:02x}   {e}")
            ng += 1
            continue
        pn, r = supply_period(bits)
        want = (32 - nfrq) / 2
        frac = grid_fraction(run_lengths(bits), pn)
        sat = " (飽和)" if want < 1.0 else ""
        print(f"  {nfrq:02x}   {want:8.1f}   {pn:8.3f}   {pn / want:9.3f}   "
              f"{r:8.5f}   {frac * 100:6.1f}   {len(bits):8d}{sat}")

    if want_period:
        section("[O] ノイズ系列の周期")
        print("更新周期の真ん中でサンプリングしてステップ列に直し、")
        print("先頭 64 ステップのパターンが次に現れる位置を探す。")
        print("  キャプチャが系列 1 周ぶんに足りない条件では見つからない。")
        print()
        print("nfrq   実測 P_n   取れた steps   周期 [steps]   一致率")
        for path in files:
            m = NAME_RE.search(path.name)
            if not m:
                continue
            nfrq = int(m.group(1), 16) & 0x1F
            try:
                bits, _ = load_bits(path)
            except Exception:                         # noqa: BLE001
                continue
            pn, _ = supply_period(bits)
            steps = step_bits(bits, pn)
            lag, hit = sequence_period(steps)
            got = f"{lag:12d}   {hit * 100:5.1f}%" if lag else \
                  f"{'見つからず':>12}   {'-':>6}"
            print(f"  {nfrq:02x}   {pn:8.3f}   {len(steps):12d}   {got}")

        report_recurrence(files)
        report_decimation(files)
    return 1 if ng else 0


def berlekamp_massey(s):
    """GF(2) 上で列を作る最短の線形漸化式を求める。

    返すのは (段数 L, 接続多項式の係数)。係数 `c` は `c[0] = 1` で、
    漸化式は `s[i] = c[1] s[i-1] ^ ... ^ c[L] s[i-L]`。
    **タップ位置を仮定せずに列そのものから決められる**のが要点で、
    どこにタップがあるかを当てて確かめるのとは別物。
    """
    n = len(s)
    c, b = [0] * n, [0] * n
    c[0] = b[0] = 1
    L, m = 0, -1
    for i in range(n):
        d = s[i]
        for j in range(1, L + 1):
            d ^= c[j] & s[i - j]
        if d:
            t = c[:]
            for j in range(n - (i - m)):
                c[j + i - m] ^= b[j]
            if 2 * L <= i:
                L, m, b = i + 1 - L, i, t
    return L, c[:L + 1]


def lfsr_bits(seed, count, taps=LFSR_TAPS, width=LFSR_WIDTH):
    """17 段 LFSR で `count` ビット生成する。README §5.3 の C 実装と同じもの。"""
    mask = (1 << width) - 1
    s, out = seed & mask, []
    for _ in range(count):
        bit = 0
        for t in taps:
            bit ^= (s >> (t - 1)) & 1
        s = ((s << 1) | bit) & mask
        out.append(bit)
    return out


def seed_from(bits, width=LFSR_WIDTH):
    """列の先頭 `width` ビットを `lfsr_bits` の初期値に詰め直す。"""
    return sum(bits[width - 1 - i] << i for i in range(width))


def report_recurrence(files):
    """[Q] 列を作る漸化式を同定し、全長で検証し、生成した列と突き合わせる。"""
    section("[Q] 系列を作る漸化式")
    print("列そのものから最短の線形漸化式を求め（タップ位置は仮定しない）、")
    print("全長で成り立つかを見る。さらに先頭 17 ビットだけを種にして生成した列と")
    print("実測の列を 1 ビットずつ突き合わせる。")
    print(f"  DAC の値が高い側が 0、低い側が 1（`y` の極性）。")
    print()
    print("nfrq     steps   同定した段数   同定したタップ   漸化式の不成立   "
          "生成列との不一致")
    for path in files:
        m = NAME_RE.search(path.name)
        if not m:
            continue
        nfrq = int(m.group(1), 16) & 0x1F
        try:
            bits, _ = load_bits(path)
        except Exception:                             # noqa: BLE001
            continue
        pn, _ = supply_period(bits)
        # DAC の高い側を 0 に取る。この極性でだけ 2 タップの式になる
        y = [b ^ 1 for b in step_bits(bits, pn)]
        if len(y) < 4 * LFSR_WIDTH:
            continue
        lo, c = berlekamp_massey(y[:BM_SAMPLE])
        taps = tuple(i for i, v in enumerate(c) if v and i > 0)
        ng = sum(1 for i in range(lo, len(y))
                 if y[i] != _recur(y, i, taps))
        gen = lfsr_bits(seed_from(y), len(y) - LFSR_WIDTH)
        bad = sum(1 for i, v in enumerate(gen) if v != y[LFSR_WIDTH + i])
        print(f"  {nfrq:02x}  {len(y):9d}   {lo:12d}   {str(list(taps)):>14}   "
              f"{ng:14d}   {bad:16d}")


def _recur(y, i, taps):
    d = 0
    for t in taps:
        d ^= y[i - t]
    return d


def find_pattern(hay, needle, window=SEQ_WINDOW):
    """needle の先頭 window ビットが hay のどこに現れるかを返す。無ければ None。"""
    if len(needle) < window or len(hay) < window:
        return None
    key, mask, cur = 0, (1 << window) - 1, 0
    for b in needle[:window]:
        key = (key << 1) | b
    for i, b in enumerate(hay):
        cur = ((cur << 1) | b) & mask
        if i >= window - 1 and cur == key:
            return i - window + 1
    return None


def report_decimation(files, ref_nfrq=0x00, targets=(0x1E, 0x1F)):
    """観測できるビット列が「間引き方」を区別できないことを示す。

    どの `NFRQ` でも 1 ステップは 16 シフトぶんなので、観測列は同じ系列を
    2 の冪で間引いたものになる。**この系列は 2 の冪で間引いても自分自身の
    巡回シフトになる**ため、1 個おきに間引いた基準列とも 2 個おきに間引いた
    基準列とも一致してしまう。つまり `NFRQ=0x1f` が 1 サンプルに 1 回進むのか
    2 回進むのかは、この観測量からは決められない。
    """
    def load_steps(nfrq):
        for path in files:
            m = NAME_RE.search(path.name)
            if m and (int(m.group(1), 16) & 0x1F) == nfrq:
                bits, _ = load_bits(path)
                pn, _ = supply_period(bits)
                return step_bits(bits, pn)
        return None

    ref = load_steps(ref_nfrq)
    if ref is None:
        return
    section("[P] 観測列は間引き方を区別できない")
    print(f"NFRQ={ref_nfrq:02x} のステップ列を基準にし、1 個おき / 2 個おきに"
          "間引いたものと突き合わせる。")
    print("  どちらでも一致するなら、観測列からは 1 サンプルあたりの更新回数を"
          "復元できない。")
    print()
    print("nfrq   間引き   一致した位置   一致率")
    for nfrq in targets:
        tgt = load_steps(nfrq)
        if tgt is None:
            continue
        for stride in (1, 2):
            best = None
            for ph in range(stride):
                dec = ref[ph::stride]
                pos = find_pattern(dec, tgt)
                if pos is None:
                    continue
                m = min(len(dec) - pos, len(tgt))
                same = sum(1 for j in range(m) if dec[pos + j] == tgt[j])
                if best is None or m > best[2]:
                    best = (pos, same, m)
            if best is None:
                print(f"  {nfrq:02x}   {stride} 個おき   {'見つからず':>12}   -")
            else:
                pos, same, m = best
                print(f"  {nfrq:02x}   {stride} 個おき   {pos:12d}   "
                      f"{same / m * 100:6.2f}%  ({same}/{m})")


# ---- 自己検証 -----------------------------------------------------------

def synth_bits(n, period, seed=1234):
    """更新周期 period [samples] で符号が変わりうる 2 値列を作る。

    値そのものは独立なコイン投げ。実機と同じく「更新の瞬間にだけ 1/2 で反転する」
    構造になるので、変化率からの P_n 推定と連長の整数倍性を同時に検証できる。
    """
    rng = random.Random(seed)
    out, cur, nxt = [], rng.randint(0, 1), period
    for i in range(n):
        while i >= nxt:
            cur = rng.randint(0, 1)
            nxt += period
        out.append(cur)
    return out


def self_test():
    """人工信号で P_n の推定と系列の周期探索を検証する（実機不要）。"""
    results = []
    n = 1 << 19
    for period in (16.0, 15.5, 8.0, 4.0, 2.0, 1.0):
        bits = synth_bits(n, period)
        pn, _ = supply_period(bits)
        err = None
        if pn is None:
            err = "推定できない"
        elif abs(pn - period) / period > 0.05:
            err = f"P_n {pn:.3f} (期待 {period:g})"
        else:
            frac = grid_fraction(run_lengths(bits), pn)
            if period >= 2.0 and frac < 0.95:
                err = f"連長の整数倍% が低い: {frac * 100:.1f}"
        results.append((f"P_n {period:g} の推定", err))

    # 飽和側: 1 サンプルに複数回更新が入ると r は 0.5 で頭打ち
    bits = synth_bits(n, 0.5)
    pn, r = supply_period(bits)
    results.append(("P_n 0.5 は飽和して 1 サンプルより下に行かない",
                    None if pn is not None and pn >= 0.9
                    else f"P_n {pn} / r {r:.3f}"))

    # ステップ列の復元。P_n の真ん中で拾えば 1 ステップ 1 ビットに戻るはず
    period, want_steps = 5.0, 4096
    rng = random.Random(3)
    src = [rng.randint(0, 1) for _ in range(want_steps)]
    bits = [src[int(i / period)] for i in range(int(period * want_steps))]
    got = step_bits(bits, period)
    bad = sum(1 for a, b in zip(src, got) if a != b)
    results.append(("ステップ列の復元",
                    None if bad == 0 else f"{bad}/{len(got)} ステップ食い違う"))

    # 系列の周期。既知の周期で繰り返すステップ列を作って戻るか見る
    want = 3000
    rng = random.Random(7)
    base = [rng.randint(0, 1) for _ in range(want)]
    lag, hit = sequence_period((base * 3)[:int(want * 2.5)])
    results.append((f"系列の周期 {want} steps を見つける",
                    None if lag == want else f"見つけたのは {lag} (一致 {hit:.3f})"))

    # 周期を持たない列では見つからないこと
    rng = random.Random(9)
    lag, _ = sequence_period([rng.randint(0, 1) for _ in range(1 << 14)])
    results.append(("周期の無い列では見つけない",
                    None if lag is None else f"{lag} を見つけてしまった"))

    # 漸化式の同定。既知のタップで作った列から、そのタップを言い当てられるか
    for taps in (LFSR_TAPS, (11, 15)):
        gen = lfsr_bits(1, 4096, taps=taps, width=max(taps))
        lo, c = berlekamp_massey(gen[:512])
        got = tuple(i for i, v in enumerate(c) if v and i > 0)
        results.append((f"漸化式の同定（タップ {list(taps)}）",
                        None if got == taps else f"同定結果 {list(got)}"))

    # 17 段 / タップ 14,17 が最大長（周期 2^17-1）であること
    gen = lfsr_bits(1, (1 << 17) + 64, taps=LFSR_TAPS, width=LFSR_WIDTH)
    lag, _ = sequence_period(gen)
    results.append(("17 段 / タップ 14,17 の周期が 2^17-1",
                    None if lag == (1 << 17) - 1 else f"周期 {lag}"))

    # 生成器と種の詰め方が噛み合っていること
    ref = lfsr_bits(0x1234, 2048, taps=LFSR_TAPS, width=LFSR_WIDTH)
    again = lfsr_bits(seed_from(ref), len(ref) - LFSR_WIDTH, taps=LFSR_TAPS,
                      width=LFSR_WIDTH)
    bad = sum(1 for i, v in enumerate(again) if v != ref[LFSR_WIDTH + i])
    results.append(("先頭 17 ビットを種にすると続きを再現できる",
                    None if bad == 0 else f"{bad} ビット食い違う"))

    ng = 0
    for name, err in results:
        if err is None:
            print(f"PASS {name}")
        else:
            print(f"FAIL {name}: {err}")
            ng += 1
    print(f"--- {len(results)} ケース / NG {ng}")
    return 1 if ng else 0


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="NE で直接出したノイズから発生器の周期と系列を測る")
    parser.add_argument("--wav-dir", type=Path, default=SCRIPT_DIR / "wav_ne",
                        help="解析するキャプチャのディレクトリ（既定 ./wav_ne）")
    parser.add_argument("--period", action="store_true",
                        help="系列の周期も探す（遅れを総当たりするので遅い）")
    parser.add_argument("--self-test", action="store_true",
                        help="人工信号で自己検証する（実機不要）")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()
    if not ANALYZER.exists():
        print(f"! 解析器が見つからない: {ANALYZER}", file=sys.stderr)
        return 1
    return report(args.wav_dir, args.period)


if __name__ == "__main__":
    sys.exit(main())
