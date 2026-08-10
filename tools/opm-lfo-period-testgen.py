#!/usr/bin/env python3
"""
`opm-lfo-period.py` の自己テスト。

既知の更新周期を持つ人工信号を作り、解析器が本当にその周期を当てるかを確かめる。
実機もロジックアナライザも要らない。

## 作る信号

`test/lfo_noise/opm_seq.txt` が鳴らしているものと同じ「4 オペレータ ALG=7 を同一
パラメータで並列加算した正弦波」を想定し、単一の正弦波として合成する。

    am: 振幅を段状に変える（AMS=2 相当の 48dB 幅）
    pm: 搬送波周波数を段状に変える（PMS=7 相当の ±400cent 幅）

LFO 値は W=3 (random) を模した疑似乱数で、**わざと近い値・同じ値を連続させる**。
ランダム LFO では隣り合う更新値がたまたま近くなることがあり、そのとき更新イベントは
ほとんど見えなくなる。それでも周期を 2 倍に取り違えないことが最大の確認点。

## 確かめること

    - 8 〜 262144 samples の更新周期を am / pm の両方で当てられる
    - 同じ更新周期で搬送波だけを変えても結果が変わらない（搬送波を前提にしていない）
    - 整数でない周期も小数のまま出る
    - .wav / .wav.zst を混ぜて複数指定できる
    - R チャネルを差し替えても結果が変わらない（L しか使っていない）
    - 標準出力が TSV だけになっている

`PASS` / `FAIL` は標準出力に出す。失敗が 1 件でもあれば終了コードは 1。

外部ライブラリは使わない（標準ライブラリのみ）。

SPDX-License-Identifier: MIT
"""

import argparse
import array
import math
import random
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

try:                                    # Python 3.14+ (PEP 784)
    from compression.zstd import compress as zstd_compress
except ImportError:                     # 3.13 以前は .zst を作れない
    zstd_compress = None

SCRIPT_DIR = Path(__file__).resolve().parent
ANALYZER = SCRIPT_DIR / "opm-lfo-period.py"

# ---- 設定 ---------------------------------------------------------------

PCM_MAX = 32000                     # 合成時の full scale
AM_DEPTH_DB = 48.0                  # AMS=2 相当
PM_DEPTH_CENTS = 400.0              # PMS=7 相当

# 1 条件あたりのサンプル数。周期の 32 倍を狙い、上下限で頭を押さえる
CYCLES = 32
LEN_MIN = 1 << 15
LEN_MAX = 1 << 22

# 実機で使っている搬送波周期の範囲を代表する 4 点 [samples]。
#   6.68 = KC 5e / MUL 8 （test/dac_lr の fast）
#  10.22 = 特定の KC / MUL には対応しない中間値
#  31.78 = KC 4a / MUL 4 （capture_lfo.py のはしごの先頭）
# 127.13 = KC 4a / MUL 1 （test/lfo_noise/wav_4a_01）
CARRIERS = (6.68, 10.22, 31.78, 127.13)
CARRIER_DEFAULT = 31.78

# 判定に使う相対誤差
TOL = 0.005

PERIODS = (8, 16, 32, 64, 128, 256, 512, 1024, 4096, 16384, 65536, 262144)


# ---- 人工信号 -----------------------------------------------------------

def lfo_values(count, seed):
    """W=3 を模したランダム列。近い値・同じ値の連続をわざと混ぜる。"""
    rng = random.Random(seed)
    vals = [rng.randrange(256)]
    for i in range(1, count):
        prev = vals[-1]
        if i % 7 == 3:                          # 完全に同じ値が続く
            vals.append(prev)
        elif i % 11 == 5:                       # 量子化 1 段だけ違う
            vals.append(min(255, prev + 1))
        elif i % 13 == 9:                       # ほぼ同じ値が 3 回続く
            vals.append(max(0, prev - 1))
        else:
            vals.append(rng.randrange(256))
    return vals


def lfo_values_close(count, seed):
    """更新値がほとんど変わらない極端な列。イベントの大半がごく小さくなる。

    大きな変化だけを拾う実装ならここで更新を見落とし、周期を 2 倍以上に取り違える。
    """
    rng = random.Random(seed)
    vals = [128]
    for _ in range(1, count):
        prev = vals[-1]
        if rng.random() < 0.15:                 # たまに大きく飛ぶ
            vals.append(rng.randrange(256))
        else:                                   # ほとんどは ±2 以内
            vals.append(max(0, min(255, prev + rng.randint(-2, 2))))
    return vals


def lfo_values_iid(count, seed):
    """繰り返しも近い値も混ぜない独立同分布の列。

    「同じ語を続けて引いたか」を測る側の検証で使う。`lfo_values` は repeat を
    わざと混ぜてあるので、真の連の長さが供給周期だけでは決まらなくなる。
    """
    rng = random.Random(seed)
    return [rng.randrange(256) for _ in range(count)]


def synth(mode, period, length, carrier, seed, close=False, noise_period=None,
          iid=False):
    """更新周期 period の人工信号を作って array('h') で返す。

    noise_period を渡すと、値の供給側（ノイズ発生器）が period とは別の周期で走る
    実機の構造を再現する。段 i が引く語は `int(i * period / noise_period)` 番目に
    なるので、noise_period > period では同じ語を続けて引く段が出る。
    """
    nseg = int(math.ceil(length / period)) + 1
    nval = nseg if noise_period is None \
        else int(nseg * period / noise_period) + 2
    if iid:
        src = lfo_values_iid(nval, seed)
    else:
        src = lfo_values_close(nval, seed) if close else lfo_values(nval, seed)
    vals = src if noise_period is None \
        else [src[int(i * period / noise_period)] for i in range(nseg)]
    w0 = 2.0 * math.pi / carrier

    out = array.array("h")
    theta = 0.0
    for i, v in enumerate(vals):
        lo = int(i * period)
        hi = min(length, int((i + 1) * period))
        n = hi - lo
        if n <= 0:
            continue
        if mode == "am":
            amp = PCM_MAX * 10.0 ** (-(v / 255.0) * AM_DEPTH_DB / 20.0)
            w = w0
        else:
            amp = float(PCM_MAX)
            w = w0 * 2.0 ** (((v / 255.0) - 0.5) * 2.0 * PM_DEPTH_CENTS / 1200.0)
        out.extend(max(-32768, min(32767, round(amp * math.cos(theta + w * k))))
                   for k in range(n))
        theta += w * n
    return out


def write_wav(path, left, right=None):
    """16bit ステレオ WAV を書く。.zst で終わる名前なら zstd で圧縮する。"""
    if right is None:                           # 実機同様、R は L の 2 タップ平均
        right = array.array("h", [(left[i] + left[min(i + 1, len(left) - 1)]) // 2
                                  for i in range(len(left))])
    inter = array.array("h", bytes(4 * len(left)))
    inter[0::2] = left
    inter[1::2] = right
    if sys.byteorder == "big":
        inter.byteswap()
    data = inter.tobytes()

    rate = 62500                                # 解析には使われない値
    blob = struct.pack("<4sL4s4sLHHLLHH4sL",
                       b"RIFF", 36 + len(data), b"WAVE",
                       b"fmt ", 16, 1, 2, rate, rate * 4, 4, 16,
                       b"data", len(data)) + data
    if str(path).endswith(".zst"):
        blob = zstd_compress(blob, 3)
    path.write_bytes(blob)


def length_for(period):
    return int(min(max(period * CYCLES, LEN_MIN), LEN_MAX))


# ---- 解析器の呼び出し ---------------------------------------------------

def run_analyzer(paths, mode, verbose):
    """解析器を走らせて TSV を dict のリストで返す。"""
    argv = [sys.executable, str(ANALYZER), "--mode", mode] + [str(p) for p in paths]
    proc = subprocess.run(argv, capture_output=True, text=True)
    if verbose and proc.stderr:
        for line in proc.stderr.splitlines():
            print(f"  | {line}")
    if proc.returncode != 0:
        raise AssertionError(f"解析器が終了コード {proc.returncode} で失敗した: "
                             f"{proc.stderr.strip()}")
    lines = proc.stdout.splitlines()
    if not lines:
        raise AssertionError("標準出力が空")
    cols = lines[0].split("\t")
    return [dict(zip(cols, ln.split("\t"))) for ln in lines[1:]], proc


def check_period(res, want, tol=TOL):
    got = float(res["period_samples"])
    if want <= 0 or abs(got - want) / want > tol:
        return f"周期 {got:.4f} (期待 {want:g})"
    n = int(res["sample_count"])
    if abs(float(res["cycle_count"]) - n / got) > 0.01:
        return f"cycle_count {res['cycle_count']} が sample_count/period と合わない"
    if not 0.0 <= float(res["confidence"]) <= 1.0:
        return f"confidence {res['confidence']} が 0〜1 の外"
    return None


# ---- 各テスト -----------------------------------------------------------

def test_periods(outdir, mode, verbose):
    """8 〜 262144 samples の各周期を当てられるか。"""
    out = []
    for period in PERIODS:
        path = outdir / f"{mode}_p{period}.wav"
        write_wav(path, synth(mode, period, length_for(period),
                              CARRIER_DEFAULT, 1000 + period))
        try:
            rows, _ = run_analyzer([path], mode, verbose)
            err = check_period(rows[0], period)
        except AssertionError as e:
            err = str(e)
        out.append((f"{mode} P={period}", err))
        path.unlink()
    return out


def test_carriers(outdir, mode, verbose):
    """同じ周期で搬送波だけを変えても結果が変わらないか。"""
    period = 256
    paths = []
    for i, tc in enumerate(CARRIERS):
        p = outdir / f"{mode}_tc{i}.wav"
        write_wav(p, synth(mode, period, length_for(period), tc, 4242))
        paths.append(p)
    try:
        rows, _ = run_analyzer(paths, mode, verbose)
        errs = [check_period(r, period) for r in rows]
        err = next((e for e in errs if e), None)
        if err is None:
            got = [float(r["period_samples"]) for r in rows]
            if (max(got) - min(got)) / period > TOL:
                err = f"搬送波で結果が変わった: {[round(v, 4) for v in got]}"
    except AssertionError as e:
        err = str(e)
    for p in paths:
        p.unlink()
    return [(f"{mode} 搬送波不変 (Tc={'/'.join(str(c) for c in CARRIERS)})", err)]


def test_fractional(outdir, mode, verbose):
    """整数でない周期が小数のまま出るか。"""
    period = 1000.5
    path = outdir / f"{mode}_frac.wav"
    write_wav(path, synth(mode, period, length_for(period), CARRIER_DEFAULT, 77))
    try:
        rows, _ = run_analyzer([path], mode, verbose)
        err = check_period(rows[0], period)
    except AssertionError as e:
        err = str(e)
    path.unlink()
    return [(f"{mode} 非整数周期 P={period}", err)]


def test_close_values(outdir, mode, verbose):
    """更新値が連続してほとんど変わらなくても周期を見失わないか。"""
    out = []
    for period in (16, 256, 4096):
        path = outdir / f"{mode}_close{period}.wav"
        write_wav(path, synth(mode, period, length_for(period),
                              CARRIER_DEFAULT, 8000 + period, close=True))
        try:
            rows, _ = run_analyzer([path], mode, verbose)
            err = check_period(rows[0], period)
        except AssertionError as e:
            err = str(e)
        out.append((f"{mode} 近い値の連続 P={period}", err))
        path.unlink()
    return out


def test_noise_hold(outdir, mode, verbose):
    """値の供給が更新より遅いときに推定周期がどうなるか。

    test/lfo_noise/README.md §2.2 の機構。供給が段のちょうど 2 倍遅いと 2 段に 1 回しか
    値が変わらず、推定周期は 2 倍になる。整数比から外れると 1 段ぶんの間隔が混じるので、
    推定周期は段の周期そのものに戻る。**この 2 つを取り違えないことが要点**で、
    「速い側の頭打ち」と「供給側との噛み合わせ」を実測で区別できる根拠になる。
    """
    period = 64
    length = 1 << 18                            # 4096 段
    out = []
    for pn, want, note in (
            (period * 2.0000, period * 2, "ちょうど 2 倍でロック"),
            (period * 1.9375, period, "2 倍からわずかに外れる"),
            (period * 1.0625, period, "1 倍からわずかに外れる"),
            (period * 0.5000, period, "供給の方が速い")):
        path = outdir / f"{mode}_hold{pn:g}.wav"
        write_wav(path, synth(mode, period, length, CARRIER_DEFAULT,
                              606, noise_period=pn))
        try:
            rows, _ = run_analyzer([path], mode, verbose)
            err = check_period(rows[0], want)
        except AssertionError as e:
            err = str(e)
        out.append((f"{mode} 供給周期 {pn:g} / 段 {period} -> P={want}（{note}）", err))
        path.unlink()
    return out


def test_left_only(outdir, mode, verbose):
    """R チャネルを別物に差し替えても結果が変わらないか。"""
    period = 512
    left = synth(mode, period, length_for(period), CARRIER_DEFAULT, 555)
    plain = outdir / f"{mode}_lr_plain.wav"
    swapped = outdir / f"{mode}_lr_other.wav"
    write_wav(plain, left)
    # R には別の周期・別の搬送波の信号を入れる
    other = synth(mode, period * 3, len(left), CARRIERS[0], 999)
    write_wav(swapped, left, other)
    try:
        rows, _ = run_analyzer([plain, swapped], mode, verbose)
        err = check_period(rows[0], period) or check_period(rows[1], period)
        if err is None and rows[0]["period_samples"] != rows[1]["period_samples"]:
            err = (f"R の中身で結果が変わった: {rows[0]['period_samples']} vs "
                   f"{rows[1]['period_samples']}")
    except AssertionError as e:
        err = str(e)
    plain.unlink()
    swapped.unlink()
    return [(f"{mode} L のみ使用", err)]


def test_io(outdir, verbose):
    """.wav と .wav.zst の混在指定、TSV の体裁、--mode の必須指定。"""
    out = []
    period = 256
    left = synth("am", period, length_for(period), CARRIER_DEFAULT, 31337)
    plain = outdir / "io_plain.wav"
    packed = outdir / "io_packed.wav.zst"
    write_wav(plain, left)

    if zstd_compress is None:
        out.append(("混在指定 (.wav + .wav.zst)", None))
        print("SKIP zstd (.wav.zst の生成): Python 3.14 以降が必要")
    else:
        write_wav(packed, left)
        try:
            rows, proc = run_analyzer([plain, packed, plain], "am", verbose)
            err = None
            if len(rows) != 3:
                err = f"3 行出るはずが {len(rows)} 行"
            if err is None:
                err = next((e for e in (check_period(r, period) for r in rows) if e),
                           None)
            if err is None and rows[0]["period_samples"] != rows[1]["period_samples"]:
                err = (".wav と .wav.zst で結果が違う: "
                       f"{rows[0]['period_samples']} vs {rows[1]['period_samples']}")
            if err is None:
                head = proc.stdout.splitlines()
                if head.count("\t".join(head[0].split("\t"))) != 1:
                    err = "ヘッダが 1 回だけになっていない"
                elif any(not ln.strip() or ln.startswith(("!", "*", "#"))
                         for ln in head):
                    err = "標準出力に TSV 以外が混ざっている"
        except AssertionError as e:
            err = str(e)
        out.append(("混在指定 (.wav + .wav.zst)", err))
        packed.unlink()

    # --mode を省略したらエラーで終わること
    proc = subprocess.run([sys.executable, str(ANALYZER), str(plain)],
                          capture_output=True, text=True)
    err = None if proc.returncode != 0 else "--mode 無しでも走ってしまった"
    if err is None and proc.stdout.strip():
        err = "--mode 無しなのに標準出力に何か出た"
    out.append(("--mode の必須指定", err))

    # 壊れた入力でも他のファイルの処理を続け、終了コードだけ 1 になること
    broken = outdir / "io_broken.wav"
    broken.write_bytes(b"not a wav file at all")
    proc = subprocess.run([sys.executable, str(ANALYZER), "--mode", "am",
                           str(broken), str(plain)],
                          capture_output=True, text=True)
    err = None
    if proc.returncode != 1:
        err = f"終了コードが {proc.returncode}"
    elif len(proc.stdout.splitlines()) != 2:
        err = "壊れた入力の後で処理が続いていない"
    elif "!" not in proc.stderr:
        err = "エラーが標準エラーに出ていない"
    out.append(("壊れた入力の扱い", err))
    broken.unlink()
    plain.unlink()
    return out


# ---- エントリポイント ---------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(
        description="opm-lfo-period.py の自己テスト")
    parser.add_argument("--mode", choices=("am", "pm"), default=None,
                        help="このモードだけ試す（既定は両方）")
    parser.add_argument("--outdir", type=Path, default=None,
                        help="生成物を残すディレクトリ（既定は一時ディレクトリ）")
    parser.add_argument("--verbose", action="store_true",
                        help="解析器の標準エラーも表示する")
    args = parser.parse_args(argv)

    if not ANALYZER.exists():
        print(f"! 解析器が見つからない: {ANALYZER}", file=sys.stderr)
        return 1

    tmp = None
    if args.outdir is None:
        tmp = tempfile.TemporaryDirectory()
        outdir = Path(tmp.name)
    else:
        outdir = args.outdir
        outdir.mkdir(parents=True, exist_ok=True)

    modes = (args.mode,) if args.mode else ("am", "pm")
    try:
        results = []
        for mode in modes:
            results += test_periods(outdir, mode, args.verbose)
            results += test_carriers(outdir, mode, args.verbose)
            results += test_fractional(outdir, mode, args.verbose)
            results += test_close_values(outdir, mode, args.verbose)
            results += test_noise_hold(outdir, mode, args.verbose)
            results += test_left_only(outdir, mode, args.verbose)
        results += test_io(outdir, args.verbose)

        ng = 0
        for name, err in results:
            if err is None:
                print(f"PASS {name}")
            else:
                print(f"FAIL {name}: {err}")
                ng += 1
        print(f"--- {len(results)} ケース / NG {ng}")
        if args.outdir is not None:
            print(f"* 生成物: {outdir}")
        return 1 if ng else 0
    finally:
        if tmp is not None:
            tmp.cleanup()


if __name__ == "__main__":
    sys.exit(main())
