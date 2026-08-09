#!/usr/bin/env python3
"""
opm-dac2wav.py の自己テスト。

既知の PCM 波形を YM3012 のシリアルフォーマットへ**エンコード**し、φ1 / SO / SH1 / SH2 の
4ch を 8MHz グリッドへ描き起こした疑似キャプチャ (.bin) を作る。それを opm-dac2wav.py に
CLI 越しに食わせ、出てきた WAV が元の PCM と完全一致するかを確かめる。

これでビット順・SH1/SH2 の対応・冒頭の切り捨て・位相の自動判定までまとめて回帰検査できる。

入出力の経路（ファイル / 標準入出力 / zstd 圧縮）も同じ疑似キャプチャで検査する。
実運用では `sigrok-cli | opm-dac2wav.py - out.wav.zst` とパイプで繋いで中間ファイルを
作らないため、ファイル経由と**バイト単位で一致する**ことが前提になる。

疑似キャプチャには実機に寄せた次の細工を入れてある:

- キャプチャ開始位置をランダムにずらす（フレーム途中・サイクル途中から始まる）
- φ1 と LA のサンプルレートをわずかにずらし、1 相あたりのサンプル数が 1〜3 に揺れるようにする
- 無効スロット 3bit にはランダム値を詰める（デコーダが無視することの確認）

外部ライブラリは使わない（標準ライブラリのみ）。

SPDX-License-Identifier: MIT
"""

import argparse
import array
import math
import random
import subprocess
import sys
import tempfile
import wave
from pathlib import Path

try:                                    # Python 3.14+ (PEP 784)
    from compression.zstd import decompress
except ImportError:                     # 3.13 以前は .zst のケースを飛ばす
    decompress = None

SCRIPT_DIR = Path(__file__).resolve().parent
DECODER = SCRIPT_DIR / "opm-dac2wav.py"

LA_RATE = 8_000_000.0
PHIM = 4_000_000.0
FRAME_RATE = round(PHIM / 64)     # 62500Hz

PCM_MIN = -32768
PCM_MAX = 32704                   # 511 << 6


# ---- エンコード ---------------------------------------------------------

def encode(v):
    """int16 → (仮数 10bit オフセットバイナリ, 指数 1..7)。指数は表現できる最小のものを選ぶ。"""
    if not PCM_MIN <= v <= PCM_MAX:
        raise ValueError(f"表現できない値: {v}")
    for e in range(1, 8):
        m = (v >> (e - 1)) + 512
        if 0 <= m <= 1023:
            return m, e
    raise ValueError(v)


def decode(m, e):
    return (m - 512) << (e - 1)


def quantize(v):
    """エンコード → デコードを通した値（切り捨て後の期待値）。"""
    m, e = encode(max(PCM_MIN, min(PCM_MAX, v)))
    return decode(m, e)


def cell_stream(frames, rng):
    """フレーム列 [(L, R), ...] を φ1 1 クロック 1 バイトの (SO, SH1, SH2) 列へ。

    ビット割り当ては SO=bit0 / SH1=bit1 / SH2=bit2。
    1 フレーム 32 クロックの中身は CH1 の D0 を起点に
    `D0..D9, S0..S2, 無効×3, D0..D9, S0..S2, 無効×3`。
    SH2 は CH1 の S2 で、SH1 は CH2 の S2 で立ち下がる（H 区間は 8 クロック）。
    """
    cells = bytearray(32 * len(frames))
    for i, (left, right) in enumerate(frames):
        bits = []
        for v in (left, right):
            m, e = encode(v)
            bits += [(m >> k) & 1 for k in range(10)]
            bits += [(e >> k) & 1 for k in range(3)]
            bits += [rng.getrandbits(1) for _ in range(3)]
        base = 32 * i
        for c, so in enumerate(bits):
            sh2 = 1 if 5 <= c <= 12 else 0
            sh1 = 1 if 21 <= c <= 28 else 0
            cells[base + c] = so | (sh1 << 1) | (sh2 << 2)
    return cells


def render(cells, rng, ppm=25.0):
    """クロック列を 8MHz のロジアナサンプル列へ描き起こす。bit0=φ1, bit1=SO, bit2=SH1, bit3=SH2。"""
    period = (LA_RATE / (PHIM / 2)) * (1.0 + ppm * 1e-6)  # 1 クロックあたりの LA サンプル数
    phase = rng.uniform(0.0, period)
    total = int((len(cells) - 1) * period + phase)
    raw = bytearray(total)
    for n in range(total):
        u = (n - phase) / period + 0.5
        k = int(u)
        if k < 0 or k >= len(cells):
            continue
        # セルの前半が L（この間にデータが確定している）、後半が H
        phi1 = 0 if (u - k) < 0.5 else 1
        raw[n] = (cells[k] << 1) | phi1
    return raw


# ---- テストケース -------------------------------------------------------

def make_frames(n_each, rng):
    """(フレーム列, 各ケースの名前と範囲) を返す。"""
    frames = []
    marks = []

    def add(name, gen):
        start = len(frames)
        for i in range(n_each):
            frames.append(gen(i))
        marks.append((name, start, len(frames)))

    add("無音", lambda i: (0, 0))
    add("フルスケール正弦",
        lambda i: (quantize(int(32000 * math.sin(2 * math.pi * 440 * i / FRAME_RATE))),
                   quantize(int(4000 * math.sin(2 * math.pi * 1234 * i / FRAME_RATE)))))
    add("極値", lambda i: (PCM_MAX if i % 2 else PCM_MIN,
                           PCM_MIN if i % 2 else PCM_MAX))
    add("微小振幅", lambda i: (quantize((i % 7) - 3), quantize(-((i % 5) - 2))))
    add("乱数", lambda i: (quantize(rng.randint(PCM_MIN, PCM_MAX)),
                           quantize(rng.randint(PCM_MIN, PCM_MAX))))
    return frames, marks


def expected_bytes(frames, swap):
    out = array.array("h")
    for left, right in frames:
        if swap:
            out.extend((right, left))
        else:
            out.extend((left, right))
    if sys.byteorder == "big":
        out.byteswap()
    return out.tobytes()


# ---- 検証 ---------------------------------------------------------------

def read_wav(path):
    with wave.open(str(path), "rb") as w:
        if (w.getnchannels(), w.getsampwidth(), w.getframerate()) != (2, 2, FRAME_RATE):
            raise AssertionError(
                f"WAV の形式が違う: {w.getnchannels()}ch / {w.getsampwidth() * 8}bit / "
                f"{w.getframerate()}Hz")
        return w.readframes(w.getnframes()), w.getnframes()


def run_decoder(argv, stdin_bytes, keep_log):
    """デコーダを 1 回走らせる。戻り値は (標準出力のバイト列, エラーの文字列)。"""
    proc = subprocess.run(argv, input=stdin_bytes, capture_output=True)
    if keep_log or proc.returncode != 0:
        for line in proc.stderr.decode("utf-8", "replace").splitlines():
            print(f"  | {line}")
    if proc.returncode != 0:
        return proc.stdout, f"デコーダが終了コード {proc.returncode} で失敗した"
    return proc.stdout, None


def check(name, binpath, wavpath, frames, swap, keep_log):
    argv = [sys.executable, str(DECODER), "-v", str(binpath), str(wavpath)]
    if swap:
        argv.append("--swap")
    _, err = run_decoder(argv, None, keep_log)
    if err is not None:
        return err

    got, n_got = read_wav(wavpath)
    want = expected_bytes(frames, swap)

    # 冒頭は同期が取れるまで捨てられるので、先頭数フレームぶんのずれを許す
    for offset in range(0, 9):
        head = offset * 4
        if head + len(got) <= len(want) and want[head:head + len(got)] == got:
            lost = offset + (len(frames) - offset - n_got)
            if n_got < len(frames) - 8:
                return f"フレームが少なすぎる: {n_got} / {len(frames)}"
            return None if lost <= 8 else f"取りこぼしが多い: {lost} フレーム"

    # どこで食い違ったか出す
    for offset in range(0, 9):
        head = offset * 4
        seg = want[head:head + len(got)]
        for i in range(0, min(len(seg), len(got)), 4):
            if seg[i:i + 4] != got[i:i + 4]:
                return (f"一致しない (offset={offset}) : フレーム {i // 4} で "
                        f"期待 {seg[i:i + 4].hex()} / 実際 {got[i:i + 4].hex()}")
    return f"一致しない（出力 {n_got} フレーム / 期待 {len(frames)} フレーム）"


def check_pipe(binpath, wavpath, keep_log):
    """標準入力 → 標準出力。ファイル → ファイルの結果とバイト単位で一致するはず。"""
    argv = [sys.executable, str(DECODER), "-v", "-", "-"]
    got, err = run_decoder(argv, binpath.read_bytes(), keep_log)
    if err is not None:
        return err
    return compare_wav(got, wavpath.read_bytes())


def check_zst(binpath, wavpath, outpath, keep_log):
    """標準入力 → .wav.zst。展開したらファイル → ファイルの結果と一致するはず。"""
    argv = [sys.executable, str(DECODER), "-v", "-", str(outpath)]
    _, err = run_decoder(argv, binpath.read_bytes(), keep_log)
    if err is not None:
        return err
    if not outpath.exists():
        return f"{outpath.name} が生成されていない"
    return compare_wav(decompress(outpath.read_bytes()), wavpath.read_bytes())


def compare_wav(got, want):
    if got == want:
        return None
    if len(got) != len(want):
        return f"長さが違う: {len(got)} / 期待 {len(want)} バイト"
    for i in range(0, len(want), 4):
        if got[i:i + 4] != want[i:i + 4]:
            head = "ヘッダ" if i < 44 else f"フレーム {(i - 44) // 4}"
            return (f"{head} (offset={i}) で違う: "
                    f"{got[i:i + 4].hex()} / 期待 {want[i:i + 4].hex()}")
    return "一致しない"


def main(argv=None):
    parser = argparse.ArgumentParser(description="opm-dac2wav.py の自己テスト")
    parser.add_argument("--frames", type=int, default=1200,
                        help="テストケース 1 種あたりのフレーム数（既定 1200）")
    parser.add_argument("--seed", type=int, default=20260808, help="乱数シード")
    parser.add_argument("--outdir", type=Path, default=None,
                        help="生成物を残すディレクトリ（既定は一時ディレクトリ）")
    parser.add_argument("--verbose", action="store_true", help="デコーダの出力も表示する")
    args = parser.parse_args(argv)

    if not DECODER.exists():
        print(f"! デコーダが見つからない: {DECODER}", file=sys.stderr)
        return 1

    rng = random.Random(args.seed)
    frames, marks = make_frames(args.frames, rng)
    cells = cell_stream(frames, rng)
    raw = render(cells, rng)

    # キャプチャ開始位置は不定 = 先頭を中途半端な位置で切る
    cut = rng.randrange(1, 4 * 32 * 3)
    raw = raw[cut:]

    print(f"* テストケース: " + " / ".join(f"{n}({b - a})" for n, a, b in marks))
    print(f"* 疑似キャプチャ {len(raw)} サンプル ({len(raw) / LA_RATE:.4f}s 相当) / "
          f"先頭 {cut} サンプルを切り落とし")

    tmp = None
    if args.outdir is None:
        tmp = tempfile.TemporaryDirectory()
        outdir = Path(tmp.name)
    else:
        outdir = args.outdir
        outdir.mkdir(parents=True, exist_ok=True)

    try:
        binpath = outdir / "selftest.bin"
        binpath.write_bytes(bytes(raw))

        failures = 0

        def report(name, err):
            nonlocal failures
            if err is None:
                print(f"PASS {name}")
            else:
                print(f"FAIL {name}: {err}")
                failures += 1

        wavpath = outdir / "selftest.wav"
        for name, swap in (("既定 (CH1=L/CH2=R)", False), ("--swap (CH1=R/CH2=L)", True)):
            path = outdir / "selftest-swap.wav" if swap else wavpath
            report(name, check(name, binpath, path, frames, swap, args.verbose))

        # 入出力の経路。実運用のパイプ経路がファイル経由と一致することを見る
        report("パイプ (stdin → stdout)",
               check_pipe(binpath, wavpath, args.verbose))
        if decompress is None:
            print("SKIP zstd (stdin → .wav.zst): Python 3.14 以降が必要")
        else:
            report("zstd (stdin → .wav.zst)",
                   check_zst(binpath, wavpath, outdir / "selftest.wav.zst",
                             args.verbose))

        if args.outdir is not None:
            print(f"* 生成物: {outdir}")
        return 1 if failures else 0
    finally:
        if tmp is not None:
            tmp.cleanup()


if __name__ == "__main__":
    sys.exit(main())
