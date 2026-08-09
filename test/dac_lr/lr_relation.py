#!/usr/bin/env python3
"""
ステレオ WAV の 2 チャネルの関係を判定する。

`tools/opm-dac2wav.py` が出す WAV の L (CH1) と R (CH2) が、同じ音を RL=both で鳴らして
いるにもかかわらず一致しない件を調べるための解析器。次の 4 つの仮説の残差を並べて、
どれが成り立っているかを判定する。

    b[i] = a[i]              左右が同一
    b[i] = a[i+1]            b が 1 サンプル進んでいる
    b[i] = (a[i] + a[i+1])/2 b が隣接 2 サンプルの平均
    b[i] = 0                 b が無音（RL を片側だけにしたときの期待値）

**判定の決め手は「波形が不連続に飛ぶ点」での振る舞い**。半サンプルの遅延なら段差の前後
どちらかの値になり、平均なら中点に落ちる。滑らかな正弦波の区間だけを見ても両者は
区別できないので、`|Δa|` の大きい点だけに絞った残差 (`jd*`) を別途出す。

段差が十分に無いキャプチャ（LFO を切った持続音など）では、整数値の**厳密一致**の割合
(`eq0` / `eq1`) が決め手になる。実機が同じ値を出しているなら 1.0000 になる。

出力は 1 ファイル 1 行の TSV。複数条件を並べて比較できるよう、行にラベルを付ける
`--label` と、実行を連結するための `--no-header` を持つ。
`不明`（どの仮説も当てはまらない）は正常な測定結果の 1 つなので終了コードは 0。
1 を返すのは入力が読めなかったときと `--self-test` が落ちたときだけ。

外部ライブラリは使わない（標準ライブラリのみ）。`.wav.zst` は先頭 4 バイトのマジックで
判定して透過的に展開する（Python 3.14 以降が必要）。

SPDX-License-Identifier: MIT
"""

import argparse
import array
import math
import sys

try:                                    # Python 3.14+ (PEP 784)
    from compression.zstd import ZstdError, ZstdFile
except ImportError:                     # 3.13 以前は .zst を読めない
    ZstdFile = None
    ZstdError = None

DECOMPRESS_ERRORS = (EOFError,) if ZstdError is None else (EOFError, ZstdError)

READ_BLOCK = 1 << 20                # PCM の読み出し単位 [byte]
ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"    # zstd フレームのマジック

# 残差がこれより小さければ「その仮説が成り立っている」と見なす [dB rel. rms_a]
FIT_DB = -34.0
# 2 位の仮説とこれだけ差が付いていないと判定しない [dB]
MARGIN_DB = 6.0
# 厳密一致をもって判定する割合
EXACT_RATIO = 0.999
# 段差判定に使う点数の下限。これを下回ったら全区間の残差で判定する
JUMP_MIN = 32

TSV_COLUMNS = (
    "file", "label", "frames", "rate",
    "rms_a", "rms_b", "nz_a", "nz_b",
    "eq0", "eq1", "d0_db", "d1_db", "davg_db", "frac_delay",
    "jumps", "jd0_db", "jd1_db", "jdavg_db",
    "verdict",
)


class InputError(RuntimeError):
    pass


# ---- 入力 ---------------------------------------------------------------

def _readn(fp, n):
    buf = b""
    while len(buf) < n:
        chunk = fp.read(n - len(buf))
        if not chunk:
            break
        buf += chunk
    return buf


def _skipn(fp, n):
    while n > 0:
        chunk = fp.read(min(n, READ_BLOCK))
        if not chunk:
            return
        n -= len(chunk)


class _Prefixed:
    """先に読んでしまった先頭バイト列を戻して読ませるラッパ。"""

    def __init__(self, prefix, fp):
        self.prefix = prefix
        self.fp = fp

    def read(self, n=-1):
        if not self.prefix:
            return self.fp.read(n)
        if n is None or n < 0:
            out = self.prefix + self.fp.read()
            self.prefix = b""
            return out
        out = self.prefix[:n]
        self.prefix = self.prefix[n:]
        if len(out) < n:
            out += self.fp.read(n - len(out))
        return out

    def readable(self):
        return True


def open_input(path):
    """入力を開いて (fp, closers) を返す。zstd 圧縮なら透過的に展開する。"""
    if path in ("-", None):
        raw, closers = sys.stdin.buffer, []
    else:
        raw = open(path, "rb")
        closers = [raw]

    head = _readn(raw, len(ZSTD_MAGIC))
    fp = _Prefixed(head, raw)
    if head == ZSTD_MAGIC:
        if ZstdFile is None:
            for c in reversed(closers):
                c.close()
            raise InputError("zstd 圧縮の入力には Python 3.14 以降が必要"
                             "（`zstd -dc FILE | ...` で流し込む）")
        fp = ZstdFile(fp, "rb")
        closers.append(fp)
    return fp, closers


def read_wav_pair(fp, ch_a, ch_b, skip, limit):
    """WAV を読んで (rate, nch, array('h') a, array('h') b) を返す。"""
    hdr = _readn(fp, 12)
    if len(hdr) < 12 or hdr[0:4] != b"RIFF" or hdr[8:12] != b"WAVE":
        raise InputError("RIFF/WAVE ヘッダが無い")

    nch = rate = None
    while True:
        head = _readn(fp, 8)
        if len(head) < 8:
            raise InputError("data チャンクが見つからない")
        cid = head[0:4]
        csz = int.from_bytes(head[4:8], "little")

        if cid == b"fmt ":
            body = _readn(fp, csz)
            if csz & 1:
                _skipn(fp, 1)
            if len(body) < 16:
                raise InputError("fmt チャンクが短い")
            tag = int.from_bytes(body[0:2], "little")
            nch = int.from_bytes(body[2:4], "little")
            rate = int.from_bytes(body[4:8], "little")
            bits = int.from_bytes(body[14:16], "little")
            if tag == 0xFFFE and len(body) >= 26:      # WAVE_FORMAT_EXTENSIBLE
                tag = int.from_bytes(body[24:26], "little")
            if tag != 1:
                raise InputError(f"非圧縮 PCM でない (formatTag={tag})")
            if bits != 16:
                raise InputError(f"16bit PCM のみ対応 (bitsPerSample={bits})")
        elif cid == b"data":
            if nch is None:
                raise InputError("fmt チャンクより先に data が来た")
            total = None if csz in (0, 0xFFFFFFFF) else csz
            a, b = _stream_pair(fp, total, nch, ch_a, ch_b, skip, limit)
            return rate, nch, a, b
        else:
            _skipn(fp, csz + (csz & 1))


def _stream_pair(fp, total, nch, ch_a, ch_b, skip, limit):
    """data チャンクを流し読みして 2 チャネルぶんを array('h') に集める。"""
    for ch in (ch_a, ch_b):
        if ch >= nch:
            raise InputError(f"チャネル {ch} が無い (nchannels={nch})")
    frame = 2 * nch
    block = (READ_BLOCK // frame) * frame
    out_a, out_b = array.array("h"), array.array("h")
    tail = b""
    frames_seen = 0
    left = total
    swap = sys.byteorder == "big"

    while True:
        want = block if left is None else min(block, left)
        if want <= 0:
            break
        raw = fp.read(want)
        if not raw:
            break
        if left is not None:
            left -= len(raw)
        if tail:
            raw = tail + raw
        n_full = len(raw) // frame
        tail = raw[n_full * frame:]
        if n_full == 0:
            continue

        samples = array.array("h")
        samples.frombytes(raw[:n_full * frame])
        if swap:
            samples.byteswap()

        start = 0
        if frames_seen < skip:
            start = min(n_full, skip - frames_seen)
        frames_seen += n_full
        if start >= n_full:
            continue

        ca = samples[ch_a + start * nch::nch]
        cb = samples[ch_b + start * nch::nch]
        if limit is not None:
            room = limit - len(out_a)
            if room <= 0:
                break
            ca, cb = ca[:room], cb[:room]
        out_a.extend(ca)
        out_b.extend(cb)
        if limit is not None and len(out_a) >= limit:
            break

    return out_a, out_b


# ---- 解析 ---------------------------------------------------------------

def _db(x, ref):
    """x/ref を dB にする。ref が 0 なら None。"""
    if ref <= 0:
        return None
    if x <= 0:
        return -999.0
    return 20.0 * math.log10(x / ref)


def _residuals(saa, sa1a1, sbb, sab, sba1, saa1, n):
    """3 仮説の残差 rms を (d0, d1, davg) で返す。引数は各種の内積と点数。"""
    if n <= 0:
        return 0.0, 0.0, 0.0
    q0 = (sbb - 2 * sab + saa) / n
    q1 = (sbb - 2 * sba1 + sa1a1) / n
    qa = (sbb - sab - sba1 + (saa + 2 * saa1 + sa1a1) / 4.0) / n
    return (math.sqrt(max(q0, 0.0)), math.sqrt(max(q1, 0.0)),
            math.sqrt(max(qa, 0.0)))


def analyze(a, b, rate):
    """a を基準に b の関係を調べて結果の dict を返す。"""
    n = min(len(a), len(b))
    if n < 3:
        raise InputError(f"サンプルが少なすぎる ({n})")
    m = n - 1                       # a[i+1] を使うので最後の 1 点は使えない

    # 1 パスで内積をまとめて取る。以降の残差はすべてここから閉形式で出る
    saa = sa1a1 = sbb = sab = sba1 = saa1 = 0
    eq0 = eq1 = 0
    for x, x1, y in zip(a, a[1:], b):
        saa += x * x
        sa1a1 += x1 * x1
        sbb += y * y
        sab += x * y
        sba1 += x1 * y
        saa1 += x * x1
        if y == x:
            eq0 += 1
        if y == x1:
            eq1 += 1

    rms_a = math.sqrt(saa / m)
    rms_b = math.sqrt(sbb / m)
    d0, d1, davg = _residuals(saa, sa1a1, sbb, sab, sba1, saa1, m)

    # 最小二乗の分数遅延: b = (1-f)a[i] + f*a[i+1]
    num = sba1 - sab - saa1 + saa
    den = sa1a1 - 2 * saa1 + saa
    frac = num / den if den > 0 else None

    # 段差（|Δa| が大きい点）だけの残差。平均と遅延を分ける決め手
    dd_rms = math.sqrt(den / m) if m else 0.0
    thr = 4.0 * dd_rms
    jn = jsaa = jsa1a1 = jsbb = jsab = jsba1 = jsaa1 = 0
    if thr > 0:
        for x, x1, y in zip(a, a[1:], b):
            if abs(x1 - x) <= thr:
                continue
            jn += 1
            jsaa += x * x
            jsa1a1 += x1 * x1
            jsbb += y * y
            jsab += x * y
            jsba1 += x1 * y
            jsaa1 += x * x1
    jd0, jd1, jdavg = _residuals(jsaa, jsa1a1, jsbb, jsab, jsba1, jsaa1, jn)
    jrms_a = math.sqrt(jsaa / jn) if jn else 0.0

    res = {
        "frames": n,
        "rate": rate,
        "rms_a": round(rms_a, 1),
        "rms_b": round(rms_b, 1),
        "nz_a": m - a[:m].count(0),
        "nz_b": m - b[:m].count(0),
        "eq0": round(eq0 / m, 4),
        "eq1": round(eq1 / m, 4),
        "d0_db": _fmt_db(_db(d0, rms_a)),
        "d1_db": _fmt_db(_db(d1, rms_a)),
        "davg_db": _fmt_db(_db(davg, rms_a)),
        "frac_delay": "-" if frac is None else round(frac, 4),
        "jumps": jn,
        "jd0_db": _fmt_db(_db(jd0, jrms_a)),
        "jd1_db": _fmt_db(_db(jd1, jrms_a)),
        "jdavg_db": _fmt_db(_db(jdavg, jrms_a)),
    }
    res["verdict"] = _verdict(res, rms_a, rms_b, (d0, d1, davg),
                              (jd0, jd1, jdavg), jrms_a, jn)
    return res


def _fmt_db(v):
    return "-" if v is None else round(v, 1)


def _verdict(res, rms_a, rms_b, glob, jump, jrms_a, jn):
    """判定を 1 語で返す。"""
    if rms_a == 0.0 and rms_b == 0.0:
        return "silent"
    if rms_b == 0.0:
        return "b=0"
    if rms_a == 0.0:
        return "a=0"

    # 厳密一致が取れているならそれが最も強い証拠
    if res["eq0"] >= EXACT_RATIO:
        return "b=a"
    if res["eq1"] >= EXACT_RATIO:
        return "b=a[+1]"

    # 段差が十分にあるならそこだけで判定する（平均と遅延はここでしか割れない）
    if jn >= JUMP_MIN and jrms_a > 0:
        vals, ref = jump, jrms_a
    else:
        vals, ref = glob, rms_a

    names = ("b=a", "b=a[+1]", "b=avg")
    scored = sorted(zip((_db(v, ref) for v in vals), names))
    best_db, best = scored[0]
    second_db = scored[1][0]
    if best_db is None or best_db > FIT_DB:
        return "不明"
    if second_db is not None and second_db - best_db < MARGIN_DB:
        return "不明"
    return best


# ---- 自己検証 -----------------------------------------------------------

def _ym3012_quantize(v):
    """YM3012 の 10bit 仮数 + 3bit 指数へ丸めて線形値に戻す。"""
    v = max(-32768, min(32767, int(round(v))))
    for e in range(1, 8):                       # 指数が小さいほど分解能が高い
        step = 1 << (e - 1)
        m = int(round(v / step)) + 512
        if 0 <= m <= 1023:
            return (m - 512) * step
    return (1023 - 512) << 6


def _synth(n, period, seed=1):
    """段差付きの正弦波を作る。AM の刻みを模して 16 サンプルごとに振幅を変える。"""
    out = []
    state = seed
    gain = 1.0
    for i in range(n):
        if i % 16 == 0:
            state = (state * 1103515245 + 12345) & 0x7FFFFFFF
            gain = 0.05 + 0.95 * ((state >> 8) & 0xFF) / 255.0
        out.append(0.45 * 32767 * gain * math.sin(2 * math.pi * i / period))
    return out


def self_test(verbose=False):
    """合成波形で判定が期待どおりに出るかを見る。実機もロジアナも要らない。"""
    n = 20000
    cases = []
    for period in (6.7, 31.8, 127.1):
        ideal = _synth(n, period)
        a = array.array("h", (_ym3012_quantize(v) for v in ideal[:n - 1]))
        same = array.array("h", a)
        shift = array.array("h", (_ym3012_quantize(v) for v in ideal[1:n]))
        avg = array.array("h", (_ym3012_quantize((ideal[i] + ideal[i + 1]) / 2)
                                for i in range(n - 1)))
        zero = array.array("h", bytes(2 * len(a)))
        noise = array.array("h", (_ym3012_quantize(v)
                                  for v in _synth(n - 1, period * 1.37, seed=7)))
        cases += [
            (f"T={period} 同一", a, same, "b=a"),
            (f"T={period} 1 サンプル進み", a, shift, "b=a[+1]"),
            (f"T={period} 半サンプル平均", a, avg, "b=avg"),
            (f"T={period} 片側無音", a, zero, "b=0"),
            (f"T={period} 無相関", a, noise, "不明"),
        ]
    cases.append(("両側無音", array.array("h", bytes(2 * n)),
                  array.array("h", bytes(2 * n)), "silent"))

    ng = 0
    for name, a, b, want in cases:
        res = analyze(a, b, 62500)
        got = res["verdict"]
        ok = got == want
        ng += not ok
        print(f"{'PASS' if ok else 'FAIL'} {name}: verdict={got} (期待 {want})")
        if verbose or not ok:
            print(f"     eq0={res['eq0']} eq1={res['eq1']} "
                  f"d0={res['d0_db']} d1={res['d1_db']} davg={res['davg_db']} "
                  f"f={res['frac_delay']} jumps={res['jumps']} "
                  f"jd0={res['jd0_db']} jd1={res['jd1_db']} jdavg={res['jdavg_db']}",
                  file=sys.stderr)
    print(f"--- {len(cases)} ケース / NG {ng}")
    return 1 if ng else 0


# ---- 実行 ---------------------------------------------------------------

def channel_index(spec):
    s = spec.strip().lower()
    if s in ("l", "left"):
        return 0
    if s in ("r", "right"):
        return 1
    if s.isdigit():
        return int(s)
    raise argparse.ArgumentTypeError(f"チャネル指定が不正: {spec}")


def run_one(path, args):
    fp, closers = open_input(path)
    try:
        rate, _nch, a, b = read_wav_pair(fp, args.channel_a, args.channel_b,
                                         args.skip_samples, args.max_samples)
    except DECOMPRESS_ERRORS as e:
        raise InputError(f"zstd の展開に失敗: {e}") from e
    finally:
        for c in reversed(closers):
            c.close()

    res = analyze(a, b, rate)
    res["file"] = "-" if path in ("-", None) else path
    res["label"] = args.label if args.label else "-"
    return res


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="ステレオ WAV の 2 チャネルの関係"
                    "（同一 / 1 サンプル進み / 半サンプル平均 / 無音）を判定する")
    parser.add_argument("inputs", nargs="*", metavar="WAV",
                        help="WAV ファイル（zstd 圧縮の .wav.zst も可）。"
                             "省略 / '-' で標準入力")
    parser.add_argument("--channel-a", type=channel_index, default=0,
                        metavar="CH", help="基準チャネル l/r/番号（既定 l）")
    parser.add_argument("--channel-b", type=channel_index, default=1,
                        metavar="CH", help="比較チャネル l/r/番号（既定 r）")
    parser.add_argument("--skip-samples", type=int, default=4096, metavar="N",
                        help="先頭を捨てるサンプル数（既定 4096。KEY ON 直後を避ける）")
    parser.add_argument("--max-samples", type=int, default=None, metavar="N",
                        help="読み込む最大サンプル数")
    parser.add_argument("--label", default="", metavar="S",
                        help="出力行に付けるラベル（条件名など）")
    parser.add_argument("--no-header", action="store_true",
                        help="TSV ヘッダを出さない")
    parser.add_argument("--self-test", action="store_true",
                        help="合成波形で自己検証して終了する")
    parser.add_argument("-v", "--info", action="store_true",
                        help="補足を標準エラーへ出す")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test(args.info)

    inputs = args.inputs or ["-"]
    if not args.no_header:
        print("\t".join(TSV_COLUMNS))

    rc = 0
    for path in inputs:
        try:
            res = run_one(path, args)
        except (InputError, OSError) as e:
            print(f"! {path}: {e}", file=sys.stderr)
            rc = 1
            continue
        print("\t".join(str(res[c]) for c in TSV_COLUMNS))
        if args.info:
            print(f"* {res['file']}: {res['frames']} フレーム / {res['rate']}Hz / "
                  f"rms {res['rms_a']}:{res['rms_b']} / "
                  f"厳密一致 eq0={res['eq0']} eq1={res['eq1']} / "
                  f"残差 {res['d0_db']}/{res['d1_db']}/{res['davg_db']}dB / "
                  f"分数遅延 {res['frac_delay']} / "
                  f"段差 {res['jumps']} 点で "
                  f"{res['jd0_db']}/{res['jd1_db']}/{res['jdavg_db']}dB "
                  f"=> {res['verdict']}", file=sys.stderr)
    return rc


if __name__ == "__main__":
    sys.exit(main())
