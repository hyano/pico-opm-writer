#!/usr/bin/env python3
"""
YM2151 の DAC 出力 2 スロット (CH1 / CH2) の関係を調べるキャプチャ一式。

`opm_lr_seq.txt` を `tools/opm-writer.py` に流し込み、`!capture` でロジアナを回して
`wav/` に `.wav.zst` を溜める。LFO は使わず、鳴らすオペレータの組み合わせと RL だけを
振る。1 条件 0.5 秒、全 16 条件で 1 分ほど。

**中間ファイルは作らない。** ディスクに落ちるのは数 KB の `.wav.zst` だけ。

判定は `lr_relation.py` で行う（`--analyze` でキャプチャ後にそのまま流せる）。

外部ライブラリは使わない（標準ライブラリのみ）。

SPDX-License-Identifier: MIT
"""

import argparse
import shlex
import subprocess
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
OPM_WRITER = REPO_ROOT / "tools" / "opm-writer.py"
LR_RELATION = SCRIPT_DIR / "lr_relation.py"
SEQ = SCRIPT_DIR / "opm_lr_seq.txt"

# 搬送波。fast は 1 サンプルの時間差が位相差として大きく出るので単独オペレータの判別に、
# slow は逆にオペレータ間の時間差が無視できるので「隣接 2 サンプルの平均」の確認に向く。
CARRIERS = {
    "fast": {"KC": "5e", "MUL": "08"},   # 実測 9353.6Hz = 6.682 サンプル/周期
    "slow": {"KC": "4a", "MUL": "01"},   # 実測  492.0Hz = 127.03 サンプル/周期
}

MUTE = "7f"     # TL=0x7F。実質無音
LOUD = "00"     # TL=0x00。最大音量

# (名前, 搬送波, RL, 鳴らすオペレータ) — オペレータはレジスタオフセットで呼ぶ
CASES = [
    ("fast_4op_both",   "fast", "c7", ("00", "08", "10", "18")),
    ("fast_4op_left",   "fast", "47", ("00", "08", "10", "18")),
    ("fast_4op_right",  "fast", "87", ("00", "08", "10", "18")),
    ("fast_op00",       "fast", "c7", ("00",)),
    ("fast_op08",       "fast", "c7", ("08",)),
    ("fast_op10",       "fast", "c7", ("10",)),
    ("fast_op18",       "fast", "c7", ("18",)),
    ("fast_op00_op08",  "fast", "c7", ("00", "08")),
    ("fast_op00_op10",  "fast", "c7", ("00", "10")),
    ("fast_op00_op18",  "fast", "c7", ("00", "18")),
    ("slow_4op_both",   "slow", "c7", ("00", "08", "10", "18")),
    ("slow_op00",       "slow", "c7", ("00",)),
    ("slow_op10",       "slow", "c7", ("10",)),
    ("slow_op00_op08",  "slow", "c7", ("00", "08")),
    ("slow_op10_op18",  "slow", "c7", ("10", "18")),
    ("slow_op00_op10",  "slow", "c7", ("00", "10")),
]

# TL のプレースホルダ名 → レジスタオフセット
TL_KEYS = {"00": "TL0", "08": "TL1", "10": "TL2", "18": "TL3"}


def build_argv(case, out, args):
    name, carrier, rl, ops = case
    defines = dict(CARRIERS[carrier])
    defines["RL"] = rl
    defines["TIME"] = str(args.time_ms)
    for off, key in TL_KEYS.items():
        defines[key] = LOUD if off in ops else MUTE

    argv = [str(OPM_WRITER), str(SEQ), str(out)]
    for k in ("KC", "MUL", "RL", "TL0", "TL1", "TL2", "TL3", "TIME"):
        argv += ["-D", f"{k}={defines[k]}"]
    if args.device:
        argv += ["--device", args.device]
    if args.samplerate:
        argv += ["--samplerate", args.samplerate]
    argv += ["--phim", str(args.phim), "--zstd-level", str(args.zstd_level)]
    if args.dry_run:
        argv.append("-n")
    return argv


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="DAC の 2 スロットの関係を調べるキャプチャ一式を撮る")
    parser.add_argument("--wav-dir", type=Path, default=SCRIPT_DIR / "wav",
                        help="出力先（既定 ./wav）")
    parser.add_argument("--only", metavar="SUBSTR",
                        help="名前にこの文字列を含む条件だけ撮る")
    parser.add_argument("--time-ms", type=int, default=500, metavar="MS",
                        help="1 条件のキャプチャ長 [ms]（既定 500）")
    parser.add_argument("--device", default=None,
                        help="USB CDC のデバイス（既定は opm-writer.py に任せる）")
    parser.add_argument("--samplerate", default=None,
                        help="sigrok-cli の samplerate（既定は opm-writer.py に任せる）")
    parser.add_argument("--phim", type=float, default=4000000.0, metavar="HZ",
                        help="OPM の φM [Hz]（既定 4000000）")
    parser.add_argument("--zstd-level", type=int, default=22, metavar="N",
                        help="zstd の圧縮レベル（既定 22）")
    parser.add_argument("--analyze", action="store_true",
                        help="撮り終えたあと lr_relation.py で判定まで行う")
    parser.add_argument("-n", "--dry-run", action="store_true",
                        help="実機にもロジアナにも触らず、コマンドラインを表示するだけ")
    args = parser.parse_args(argv)

    cases = [c for c in CASES if not args.only or args.only in c[0]]
    if not cases:
        print(f"! 条件が 1 つも選ばれなかった: --only {args.only}", file=sys.stderr)
        return 1
    if not args.dry_run:
        args.wav_dir.mkdir(parents=True, exist_ok=True)

    outputs = []
    for i, case in enumerate(cases, start=1):
        out = args.wav_dir / f"{case[0]}.wav.zst"
        outputs.append(out)
        cmd = build_argv(case, out, args)
        print(f"[{i}/{len(cases)}] {case[0]}", flush=True)
        if args.dry_run:
            print("  " + " ".join(shlex.quote(a) for a in cmd))
        proc = subprocess.run(cmd)
        if proc.returncode != 0:
            print(f"! {case[0]}: opm-writer.py が失敗した", file=sys.stderr)
            return 1

    if args.analyze and not args.dry_run:
        print()
        return subprocess.run([str(LR_RELATION)] + [str(o) for o in outputs]).returncode
    return 0


if __name__ == "__main__":
    sys.exit(main())
