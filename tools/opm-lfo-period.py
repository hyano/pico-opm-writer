#!/usr/bin/env python3
"""
YM2151 の実機キャプチャから LFO の更新周期をサンプル数で測る。

`test/lfo_noise/` が集めた `.wav.zst` を読み、LFO の**更新イベントが何サンプル間隔で
起きているか**を推定して 1 ファイル 1 行の TSV で出す。LFO 波形は W=3 (random) を
想定しているので、LFO の値そのものには周期性が無い。周期があるのは**更新のタイミング
だけ**であり、この解析器が測るのはそれだけである。

## 前提にしている音源条件

    1 チャネル / 4 オペレータ / ALG=7 (4 並列加算) / 4 オペレータ同一パラメータ

この条件では出力は**単一の正弦波**になり、更新イベントの間では

    x[n+1] = 2 cos(ω) x[n] - x[n-1]

を厳密に満たす。ここから搬送波周波数を外から与えずに、AM なら包絡線、PM なら局所的な
cos(ω) を取り出せる。どちらも更新イベントの所でだけ段差になる**階段状の量**なので、
その 1 階差分がイベント列（インパルス列）になる。あとはこの列の周期を測ればよい。

## 時間の単位

すべてサンプル数で扱う。WAV ヘッダのサンプリングレートは**解析に一切使わない**。

## モード

`--mode am` / `--mode pm` は必ず利用者が指定する。入力ファイル名や内容から AM/PM を
判定することはしない。

外部ライブラリは使わない（標準ライブラリのみ）。`.wav.zst` は先頭 4 バイトのマジックで
判定してメモリ上で展開する（Python 3.14 以降が必要）。中間の WAV ファイルは作らない。

SPDX-License-Identifier: MIT
"""

import argparse
import array
import cmath
import io
import math
import sys
import wave
from itertools import accumulate
from pathlib import Path

try:                                    # Python 3.14+ (PEP 784)
    from compression.zstd import ZstdError, decompress as zstd_decompress
except ImportError:                     # 3.13 以前は .zst を読めない
    zstd_decompress = None
    ZstdError = None

DECOMPRESS_ERRORS = ((EOFError, ValueError) if ZstdError is None
                     else (EOFError, ValueError, ZstdError))

# ---- 設定 ---------------------------------------------------------------

ZSTD_MAGIC = b"\x28\xb5\x2f\xfd"

# 探索する更新周期の範囲 [samples]
PERIOD_MIN = 8.0
PERIOD_MAX = 262144.0

# ブロック長 s は「周期の 1/32」を狙う。イベントが 1 ブロックに収まり、かつ
# 1 周期を 32 ビンで折り返せる。
BLOCK_RATIO = 32
# 各スケールが担当する周期の範囲 [ブロック数]
Q_LO_BLOCKS = 8
Q_HI_BLOCKS = 64
# 周期推定に最低限必要なサイクル数
CYCLES_MIN = 3.0
# スケールを使うために必要なブロック数
NB_MIN = 24

# 粗探索・検証・歩き上がり・精密化で使うブロック数
NB_COARSE = 256
NB_VERIFY = 1024
NB_WALK = 4096
NB_REFINE = 1 << 16
# 粗探索の対数刻み（粗探索でのピーク幅 ≒ 1/(2·サイクル数) より細かくする）
COARSE_STEP = 1.015
# 局所走査の分割数と、走査窓の広さの上限（候補周期に対する比）。
# 上限が無いとサイクル数の少ない条件で窓が広がりすぎ、候補の近傍を外れて別の周期
# （多くは元の周期そのもの）を拾ってしまう。倍・3 倍の候補と混ざらない幅に抑える。
TUNE_STEPS = 48
TUNE_HALF_MAX = 0.25
# 粗探索から検証へ回す候補の数
COARSE_KEEP = 3

# 2 倍・3 倍へ歩き上がる条件（真の周期の約数を掴んでいる場合の救済）。
# 約数から真の周期へ上がるときは集中度が「上がる」（実測 0.95〜3.5 倍）。真の周期から
# さらに上がろうとすると落ちる（実測 0.30 倍以下、イベントが極端に少ない合成波形でも
# 0.79 倍）。その間に閾値を置く。
STEP_ABS = 0.30
STEP_REL = 0.88

# 搬送波の cos(ω) 推定に使うチャンク（振幅の大小に偏らないよう全体に散らす）
OMEGA_CHUNKS = 16
OMEGA_CHUNK = 1 << 14

# 位相ドリフト法の分割数と、1 区間に必要なサイクル数
FIT_SPLITS = (2, 4, 8, 16, 32)
FIT_CYCLES_MIN = 4.0
# 一致度として採用するのに必要な分割数（2 点や 3 点では直線が必ず通ってしまう）
FIT_SPLITS_MIN = 4
# 精密化はイベント検出より細かいブロックで行う（粗いブロックは位相を偏らせる）
REFINE_SHIFT = 2

# confidence に掛けるデータ量の係数が 1 になるサイクル数
CONF_CYCLES_FULL = 8.0

# 周期が搬送波の k/2 倍に近いと、搬送波由来の成分と区別が付かない。この範囲で
# 警告する（倍数が大きくなればいくらでも近い k/2 があるので上限を設ける）
RESONANCE_TOL = 0.04
RESONANCE_MAX = 8.0

TSV_COLUMNS = ("file", "mode", "channel", "sample_count",
               "period_samples", "cycle_count", "confidence")


class InputError(RuntimeError):
    pass


class AnalysisError(RuntimeError):
    pass


# ---- 入力 ---------------------------------------------------------------

def load_left(path):
    """入力を読んで (L チャネルの array('h'), チャネル数) を返す。

    `.wav.zst` はメモリ上で展開してそのまま `wave` に食わせる。中間 WAV は作らない。
    """
    raw = Path(path).read_bytes()
    if raw[:4] == ZSTD_MAGIC:
        if zstd_decompress is None:
            raise InputError("zstd 圧縮の入力には Python 3.14 以降が必要"
                             "（`zstd -dc FILE > FILE.wav` で展開する）")
        try:
            raw = zstd_decompress(raw)
        except DECOMPRESS_ERRORS as e:
            raise InputError(f"zstd の展開に失敗した: {e}") from None

    try:
        with wave.open(io.BytesIO(raw), "rb") as w:
            nch = w.getnchannels()
            width = w.getsampwidth()
            frames = w.getnframes()
            pcm = w.readframes(frames)
    except (wave.Error, EOFError) as e:
        raise InputError(f"WAV として読めない: {e}") from None

    if width != 2:
        raise InputError(f"16bit PCM ではない (sampwidth={width})")
    if nch < 1:
        raise InputError("チャネルが無い")

    samples = array.array("h")
    step = 2 * nch
    samples.frombytes(pcm[:len(pcm) - len(pcm) % step])
    if sys.byteorder == "big":
        samples.byteswap()
    return samples[0::nch], nch


# ---- 特徴量 -------------------------------------------------------------

def cos_omega(x):
    """一定振幅の正弦波としての cos(ω) を最小二乗で求める。

    x[n-1] + x[n+1] = 2 cos(ω) x[n] を全点で解く。AM では振幅が 40dB 以上振れるので、
    どこか 1 箇所ではなくファイル全体に散らしたチャンクを足し合わせる。
    """
    n = len(x)
    if n < 64:
        raise AnalysisError("サンプル数が少なすぎる")

    span = min(n, OMEGA_CHUNK)
    starts = [0] if n <= span else [
        (n - span) * i // (OMEGA_CHUNKS - 1) for i in range(OMEGA_CHUNKS)]

    num = den = 0.0
    for st in sorted(set(starts)):
        seg = x[st:st + span]
        num += sum(b * (a + c) for a, b, c in zip(seg, seg[1:], seg[2:]))
        den += sum(b * b for b in seg[1:-1])
    if den <= 0.0:
        raise AnalysisError("信号が無音")

    c = num / (2.0 * den)
    if not -0.99999 < c < 0.99999:
        raise AnalysisError(f"正弦波として解けない (cos(ω)={c:.6f})")
    return c


def build_sums(x, mode, c0):
    """スケール横断で使う累積和を 1 パスで作る。

    am: Σ env²  (env は厳密な直交成分から作った包絡線)
    pm: Σ x[n](x[n-1]+x[n+1]) と Σ x[n]²  (ブロック長 s での最小二乗の分子と分母)
    """
    if mode == "am":
        k = 1.0 / (2.0 * math.sin(math.acos(c0)))
        gen = (float(b) * b + ((c - a) * k) ** 2
               for a, b, c in zip(x, x[1:], x[2:]))
        return (array.array("d", accumulate(gen, initial=0.0)),)

    p = array.array("d", accumulate(
        (float(b) * (a + c) for a, b, c in zip(x, x[1:], x[2:])), initial=0.0))
    q = array.array("d", accumulate(
        (float(b) * b for b in x[1:-1]), initial=0.0))
    return (p, q)


def level_feature(sums, mode, s, nb):
    """ブロック長 s・ブロック数 nb での階段状特徴量の 1 階差分を返す。

    これが LFO 更新イベントの列になる。s を周期に応じて変えるのが要で、短周期は
    s=1（サンプル単位の分解能）、長周期は s≒P/32（平均化で背景ノイズが落ちる）。

    PM の cos(ω) は零交差の近くで分母が消えて値が暴れる。そこは推定が効いていない
    だけなので、ブロックのエネルギー（= 推定の効き具合）を重みにして潰す。搬送波
    位相に由来する偽のイベントが消え、倍周期での崩れ方がはっきりする。
    """
    if mode == "am":
        acc = sums[0]
        g = [acc[(i + 1) * s] - acc[i * s] for i in range(nb)]
        g = [0.5 * math.log(v) if v > 0.0 else 0.0 for v in g]
        return [abs(b - a) for a, b in zip(g, g[1:])]

    p, q = sums
    num = [p[(i + 1) * s] - p[i * s] for i in range(nb)]
    den = [q[(i + 1) * s] - q[i * s] for i in range(nb)]
    scale = sum(den) / nb
    if scale <= 0.0:
        return [0.0] * max(0, nb - 1)
    g = [n / (2.0 * v) if v > 0.0 else 0.0 for n, v in zip(num, den)]
    return [abs(b - a) * min(u, v) / scale
            for a, b, u, v in zip(g, g[1:], den, den[1:])]


# ---- 周期の探索 ---------------------------------------------------------

def fold_score(d, qb):
    """周期 qb [ブロック] で折り返した 1 次高調波の集中度と位相。

    イベントが周期 qb で並んでいれば 1 に近づく。**値そのものではなくタイミング**
    だけを見ているので、W=3 のランダムな LFO 値でも成立する。
    """
    w = -2.0 * math.pi / qb
    acc = 0j
    tot = 0.0
    for j, v in enumerate(d):
        if v:
            tot += v
            acc += v * cmath.rect(1.0, w * j)
    if tot <= 0.0:
        return 0.0, 0.0
    return abs(acc) / tot, cmath.phase(acc)


def phase_fit(d, qb, k):
    """d を k 区間に分けて 1 次高調波の位相を測り、その直線あてはめで qb を補正する。

    真の周期が qb からずれていると折り返し位相が区間ごとに一定の割合で流れる。その
    傾き β から 1/P = 1/qb + β/2π で周期が出る。残差の位相ベクトル和が「複数区間で
    同じ周期が得られているか」の一致度になる。
    """
    n = len(d)
    seg = n // k
    if seg < FIT_CYCLES_MIN * qb:
        return qb, None

    w = -2.0 * math.pi / qb
    centers, phases = [], []
    for j in range(k):
        lo, hi = j * seg, (j + 1) * seg
        acc = 0j
        for i in range(lo, hi):
            v = d[i]
            if v:
                acc += v * cmath.rect(1.0, w * i)
        if acc == 0j:
            return qb, None
        centers.append((lo + hi - 1) / 2.0)
        phases.append(cmath.phase(acc))

    for j in range(1, k):                       # アンラップ
        while phases[j] - phases[j - 1] > math.pi:
            phases[j] -= 2.0 * math.pi
        while phases[j] - phases[j - 1] < -math.pi:
            phases[j] += 2.0 * math.pi

    mx = sum(centers) / k
    my = sum(phases) / k
    sxx = sum((c - mx) ** 2 for c in centers)
    if sxx <= 0.0:
        return qb, None
    beta = sum((c - mx) * (p - my) for c, p in zip(centers, phases)) / sxx

    resid = [p - (my + beta * (c - mx)) for c, p in zip(centers, phases)]
    consist = abs(sum(cmath.rect(1.0, r) for r in resid)) / k

    inv = 1.0 / qb + beta / (2.0 * math.pi)
    if inv <= 0.0:
        return qb, consist
    return 1.0 / inv, consist


class Analyzer:
    """1 ファイル分の解析。累積和を 1 度だけ作り、あとは全部その上で動く。"""

    def __init__(self, x, mode):
        self.mode = mode
        self.c0 = cos_omega(x)
        self.carrier = 2.0 * math.pi / math.acos(self.c0)
        self.sums = build_sums(x, mode, self.c0)
        self.m = len(self.sums[0]) - 1          # 特徴量の総点数
        self.cache = {}
        self.q_max = min(PERIOD_MAX, self.m / CYCLES_MIN)
        if self.q_max < PERIOD_MIN:
            raise AnalysisError("サンプル数が少なすぎる")

    def feature(self, s, nb):
        nb = min(nb, self.m // s)
        if nb < 4:
            return []
        have = self.cache.get(s)
        if have is not None and have[0] >= nb:
            return have[1][:nb - 1]
        d = level_feature(self.sums, self.mode, s, nb)
        self.cache[s] = (nb, d)
        return d

    def level_for(self, q):
        """周期 q を測るのに適したブロック長（2 の冪）。"""
        s = 1 << max(0, round(math.log2(max(q, 1.0) / BLOCK_RATIO)))
        while s > 1 and self.m // s < NB_MIN:
            s >>= 1
        return s

    def score_at(self, s, q, nb):
        d = self.feature(s, nb)
        qb = q / s
        if len(d) < NB_MIN or qb < 4.0:
            return 0.0
        return fold_score(d, qb)[0]

    def tune(self, s, q, nb):
        """レベル s の上で q の近傍を細かく走査し、(score, q) を返す。"""
        d = self.feature(s, nb)
        qb = q / s
        if len(d) < NB_MIN or qb < 4.0:
            return 0.0, q
        if qb * CYCLES_MIN > len(d):        # この候補を測れるだけの長さが無い
            return 0.0, q
        cycles = max(len(d) / qb, 1.0)
        half = min(4.0 * qb / cycles, TUNE_HALF_MAX * qb)
        lo = max(4.0, qb - half)
        hi = min(len(d) / CYCLES_MIN, qb + half)
        if hi <= lo:
            return fold_score(d, qb)[0], q
        best = (0.0, qb)
        for i in range(TUNE_STEPS + 1):
            t = lo + (hi - lo) * i / TUNE_STEPS
            sc = fold_score(d, t)[0]
            if sc > best[0]:
                best = (sc, t)
        return best[0], best[1] * s

    def coarse(self):
        """全スケールを粗く走査して候補を集め、上位だけ多めのデータで検証する。"""
        found = []
        s = 1
        while s * Q_LO_BLOCKS <= PERIOD_MAX and self.m // s >= NB_MIN:
            d = self.feature(s, NB_COARSE)
            if len(d) >= NB_MIN:
                qb_lo = max(PERIOD_MIN, Q_LO_BLOCKS * s) / s
                qb_hi = min(PERIOD_MAX, Q_HI_BLOCKS * s, self.q_max) / s
                qb_hi = min(qb_hi, len(d) / CYCLES_MIN)
                cand = []
                qb = qb_lo
                while qb <= qb_hi:
                    cand.append((fold_score(d, qb)[0], qb))
                    qb *= COARSE_STEP
                cand.sort(reverse=True)
                seen = []
                for sc, qb in cand:
                    if any(abs(math.log(qb / t)) < 0.10 for t in seen):
                        continue
                    seen.append(qb)
                    found.append(self.tune(s, qb * s, NB_VERIFY) + (s,))
                    if len(seen) >= COARSE_KEEP:
                        break
            s *= 4
        if not found:
            raise AnalysisError("周期の候補が見つからない")
        found.sort(reverse=True)
        return found

    def walk_up(self, q, log):
        """真の周期の約数を掴んでいることがあるので 2 倍・3 倍へ歩き上がる。

        イベントが間隔 P で並んでいるとき、P の約数で折り返しても全部同じビンに落ちる
        ので集中度は高いままになる。逆に P の倍数で折り返すと等間隔の複数ビンに割れて
        1 次高調波が打ち消し合い、集中度が崩れる。したがって
        「集中度が高いまま到達できる最大の周期」が真の周期になる。

        比較は必ず**今の q に合わせた（細かい方の）ブロック長**の上で行う。倍側の粗い
        ブロックで比べると小さい周期だけがぼけて不利になり、上がりすぎる。
        """
        for _ in range(24):
            s = self.level_for(q)
            base = self.score_at(s, q, NB_WALK)
            nxt = None
            for k in (2, 3):
                q2 = q * k
                if q2 > self.q_max:
                    continue
                sc2, q2t = self.tune(s, q2, NB_WALK)
                log(f"  歩き上がり x{k}: {q:.4g} ({base:.3f}) -> "
                    f"{q2t:.4g} ({sc2:.3f})")
                if sc2 >= STEP_ABS and sc2 >= STEP_REL * base:
                    nxt = q2t
                    break
            if nxt is None:
                return q
            q = nxt
        return q

    def finalize(self, q, log):
        """精密化と confidence の算出。"""
        s = self.level_for(q)
        align, q = self.tune(s, q, NB_REFINE)

        # 折り返し位相の傾きから小数部を詰める。分割数を増やしながら q を良くして
        # いくので、次の分割でのアンラップが安全になる。
        s_ref = max(1, s >> REFINE_SHIFT)
        d = self.feature(s_ref, NB_REFINE)
        consist = None
        for k in FIT_SPLITS:
            qb, c = phase_fit(d, q / s_ref, k)
            if c is None:
                break
            q = qb * s_ref
            if k >= FIT_SPLITS_MIN:
                consist = c
        align = self.score_at(s, q, NB_REFINE)

        # 倍周期候補との差。2 倍・3 倍のうち強い方と比べる
        base = self.score_at(s, q, NB_WALK)
        rival = max((self.tune(s, q * k, NB_WALK)[0]
                     for k in (2, 3) if q * k <= self.q_max), default=0.0)
        margin = 1.0 - rival / base if base > 0.0 else 0.0
        margin = min(1.0, max(0.0, margin))

        cycles = self.m / q
        enough = min(1.0, cycles / CONF_CYCLES_FULL)
        if consist is None:
            consist = enough
        conf = align * consist * margin * enough

        log(f"  s={s}/{s_ref} align={align:.4f} consist={consist:.4f} "
            f"margin={margin:.4f} cycles={cycles:.1f}")
        return q, min(1.0, max(0.0, conf))

    def run(self, log):
        cands = self.coarse()
        for sc, q, s in cands[:COARSE_KEEP]:
            log(f"  候補: {q:.6g} samples (score {sc:.3f}, s={s})")
        q = self.walk_up(cands[0][1], log)
        return self.finalize(q, log)


def analyze(path, mode, log):
    """1 ファイルを解析して TSV の 1 行分の dict を返す。"""
    left, nch = load_left(path)
    if nch != 2:
        log(f"! {path}: ステレオ 2ch ではない (nch={nch})。"
            f"先頭チャネルを L として使う", err=True)

    a = Analyzer(left, mode)
    period, conf = a.run(log)
    log(f"  搬送波 {a.carrier:.3f} samples/周期（測定値）")

    ratio = period / a.carrier
    near = round(ratio * 2.0) / 2.0
    if 0.5 <= near <= RESONANCE_MAX and abs(ratio - near) < RESONANCE_TOL:
        log(f"! {path}: 周期が搬送波の {near:g} 倍に近い。"
            f"別の KC/MUL で撮り直して確認すること", err=True)

    return {
        "file": str(path),
        "mode": mode,
        "channel": "L",
        "sample_count": len(left),
        "period_samples": f"{period:.4f}",
        "cycle_count": f"{len(left) / period:.4f}",
        "confidence": f"{conf:.4f}",
    }


# ---- エントリポイント ---------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(
        description="YM2151 のキャプチャから LFO の更新周期をサンプル数で測る")
    parser.add_argument("input", type=Path, nargs="+",
                        help="入力する WAV。.wav / .wav.zst を混ぜてよい")
    parser.add_argument("--mode", required=True, choices=("am", "pm"),
                        help="解析モード。am=振幅変調 / pm=位相変調。"
                             "ファイル名からは判定しない")
    parser.add_argument("--no-header", action="store_true",
                        help="TSV のヘッダ行を出さない")
    parser.add_argument("-v", "--info", action="store_true",
                        help="解析の途中経過を標準エラーに出す")
    args = parser.parse_args(argv)

    def log(msg, err=False):
        if err or args.info:
            print(msg, file=sys.stderr)

    if not args.no_header:
        print("\t".join(TSV_COLUMNS))

    errors = 0
    for path in args.input:
        log(f"* {path}")
        try:
            res = analyze(path, args.mode, log)
        except (InputError, AnalysisError, OSError) as e:
            print(f"! {path}: {e}", file=sys.stderr)
            errors += 1
            continue
        print("\t".join(str(res[c]) for c in TSV_COLUMNS))
        sys.stdout.flush()

    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
