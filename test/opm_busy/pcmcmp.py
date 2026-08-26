#!/usr/bin/env python3
"""2 本のキャプチャをブロック単位で整列して突き合わせる（numpy 無し）。

サンプル単位で一致させるのは無理（再生開始の位相と tick の刻みがキャプチャに
同期していない）ので、100ms のブロックごとに最良のずらし量を探して残差を見る。
レジスタ書き込みが化けていれば音そのものが変わるので、ここに大きく出る。
"""
import array, math, sys

BLK = 6250  # 100ms @62500Hz


def load(path):
    a = array.array("h")
    a.frombytes(open(path, "rb").read())
    if sys.byteorder != "little":
        a.byteswap()
    return a[0::2]  # L だけ（DAC の 2 スロットは同一にならない: test/dac_lr/）


def rms(seq):
    return math.sqrt(sum(float(v) * v for v in seq) / len(seq)) if seq else 0.0


def compare(pa, pb, span=600):
    a, b = load(pa), load(pb)
    nblk = (min(len(a), len(b)) - 2 * span) // BLK
    env_a, env_b, res, offs = [], [], [], []
    for i in range(1, nblk - 1):
        ia = i * BLK
        sa = a[ia:ia + BLK]
        best, bestd = 0, None
        for off in range(-span, span + 1, 5):
            sb = b[ia + off:ia + off + BLK]
            if len(sb) < BLK:
                continue
            d = sum((sa[k] - sb[k]) ** 2 for k in range(0, BLK, 5))
            if bestd is None or d < bestd:
                bestd, best = d, off
        sb = b[ia + best:ia + best + BLK]
        e = math.sqrt(sum((sa[k] - sb[k]) ** 2 for k in range(0, BLK, 5)) / (BLK // 5))
        env_a.append(rms(sa)); env_b.append(rms(sb)); res.append(e); offs.append(best)
    sig = sum(env_a) / len(env_a)
    err = math.sqrt(sum(r * r for r in res) / len(res))
    env_err = math.sqrt(sum((x - y) ** 2 for x, y in zip(env_a, env_b)) / len(env_a))
    print("%s vs %s   blocks %d" % (pa, pb, len(res)))
    print("   平均 RMS            : %.1f" % sig)
    print("   波形残差 RMS        : %8.1f  (信号比 %+6.1f dB)" % (err, 20 * math.log10(err / sig)))
    print("   包絡線の差 RMS      : %8.1f  (信号比 %+6.1f dB)" % (env_err, 20 * math.log10(max(env_err, 1e-9) / sig)))
    print("   ブロックずらし量    : %d..%d サンプル" % (min(offs), max(offs)))
    worst = max(range(len(res)), key=lambda i: res[i])
    print("   最悪ブロック        : #%d  残差 %.0f / RMS %.0f" % (worst, res[worst], env_a[worst]))


if __name__ == "__main__":
    compare(sys.argv[1], sys.argv[2])
