#!/usr/bin/env python3
"""
YM2151 の LFO パラメータを振りながら DAC 出力をキャプチャし、WAV を蓄積するバッチ。

- `opm_seq.txt` を `tools/opm-writer.py` に流し込み、`!capture` でロジアナを回す
- 出力名を `.wav.zst` にするので、`opm-writer.py` が
  `sigrok-cli | tools/opm-dac2wav.py` をパイプで繋ぎ、WAV を zstd で最大圧縮して書く

**中間ファイルは作らない。** ディスクに落ちるのは数百 KB の `.wav.zst` だけで、
生キャプチャも中間の WAV もパイプの中しか通らない。

測定は **AM 測定 (AMD=7f) と PM 測定 (PMD=7f) の 2 本**。モードを最も外側、次いで
KC / NFRQ、LFRQ を最内で掃引する。LFRQ は大きい方から回す（周期とキャプチャ時間の
短い条件から先にファイルが揃う）。既に .wav.zst がある場合は、最初の欠番の 1 つ手前を
上書きするところから再開する。

失敗した条件はその場で撮り直す（`--retry`）。sigrok-cli はキャプチャが途中で死んでも
終了コード 0 を返すので、`opm-writer.py` のエラーだけでなく**撮れた .wav.zst が要求より
短いかどうか**もリトライの判定に使う。使い切ったら中断する。

搬送波 (KC / MUL) は既定では LFRQ ごとに自動で選ぶ。刻みが短い条件ほど短い搬送波が
要るため（`choose_carrier` の節を参照）。`--kc` / `--mul` を指定すればその値に固定する。

LFO 波形は `--w` で選ぶ（既定 3 = ノイズ）。W≠3 のときだけファイル名に `_w_XX` が入る。

外部ライブラリは使わない（標準ライブラリのみ）。

SPDX-License-Identifier: MIT
"""

import argparse
import math
import re
import shlex
import struct
import subprocess
import sys
import time
from pathlib import Path

try:                                    # Python 3.14+ (PEP 784)
    from compression.zstd import ZstdError, ZstdFile
except ImportError:                     # 3.13 以前は長さの検査ができない
    ZstdFile = None
    ZstdError = ()                      # except 節で使えるようにしておく

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parent.parent
TOOLS_DIR = REPO_ROOT / "tools"

OPM_WRITER = TOOLS_DIR / "opm-writer.py"
OPM_DAC2WAV = TOOLS_DIR / "opm-dac2wav.py"

# LFO の想定モデル（ymfm 相当）。これ自体が本調査の検証対象なので、
# キャプチャ長の目安を出すためだけに使う。
#   位相の増分/サンプル = (16 + (LFRQ & 0x0f)) << (LFRQ >> 4)
#   位相は 30bit 中の上位 8bit なので、1 段 = 2^22 カウント
LFO_STEP_COUNTS = 1 << 22

# OPM の内部サンプルレート = φM / 64
DEFAULT_PHIM = 4000000.0

# LFO 波形 (レジスタ 0x1B の下位 2bit)。3 = ノイズ。本調査の既定
DEFAULT_W = 3

# KC 下位ニブル → オクターブ内の半音位置。ニブル & 3 == 3 は無効値なので載せない。
# C がブロックの末尾に来るのは YM2151 の仕様どおり。
KC_SEMITONE = {0: 0, 1: 1, 2: 2, 4: 3, 5: 4, 6: 5,
               8: 6, 9: 7, 10: 8, 12: 9, 13: 10, 14: 11}

# 音程の基準: KC=0x4a (A4) が φM=3.579545MHz のとき 440Hz
OPM_A4_KC = 0x4A
OPM_A4_HZ = 440.0
OPM_A4_PHIM = 3579545.0

# --- 測定モード ---------------------------------------------------------

class Mode:
    """測定モード 1 本ぶんの設定。

    LFO を振幅に載せるか位相に載せるかの違いだが、**搬送波の選び方まで含めて
    モードごとに独立に持つ**。AM と PM で最適な KC / MUL が同じである保証は無いので、
    片方だけを後から変えられるようにしてある。

    - `defines`    : opm_seq.txt のプレースホルダに流し込む -D 値
    - `ladder`     : 搬送波のはしご (KC, MUL)。長い方から選ぶ
    - `cycles_min` : 段 1 個に入るべき搬送波の周期数の下限
    - `tc_min`     : 搬送波周期 [サンプル] の下限
    - `step_floor` : 搬送波の判定に使う LFO 刻みの床 [サンプル]
    """

    def __init__(self, name, defines, ladder, cycles_min, tc_min, step_floor):
        self.name = name
        self.defines = defines
        self.ladder = ladder
        self.cycles_min = cycles_min
        self.tc_min = tc_min
        self.step_floor = step_floor

    def define_str(self):
        return " ".join(f"{k}={v}" for k, v in self.defines.items())


# 搬送波のはしご。LFRQ ごとに最適な (KC, MUL) を探すと全条件で搬送波が変わって比較に
# ならないので、少数の組に固定して長い方から選ぶ。φM=4MHz で Tc は順に
# 31.8 / 22.5 / 15.9 / 11.2 / 8.5 / 6.0 / 4.2 サンプル。
# 4a 系と 52 系は三全音（比 √2）離れているので、互いに小さい整数比にならない。
#
# cycles_min = 段 1 個に搬送波が何周期入るか。これが段ごとのゲイン推定誤差を支配する。
# 真の刻み 8 サンプル / AM 深さ 48dB の合成波で実測したゲイン誤差 (RMS / 最悪):
#   段/周期 0.94 → 1.7% / 10%      0.53 → 5.2% / 29%
#   段/周期 0.71 → 1.8% / 10%      0.25 → 9.5% / 46%
# 0.7 を割ると 3〜6 倍に跳ねる。window_cond はこの順位を再現しないので基準に使わない
# （Tc=15.9 は cond=0.99 と最良に見えるのにゲイン誤差は 7.5%）。
#
# tc_min = 搬送波が短すぎると零交差からの周期推定も高調波フィットも成立しない。
#
# step_floor = ノイズ発生器の刻みには下限がある（実測: NFRQ=1f で 8、NFRQ=00 で
# 16 サンプル）。LFRQ=0xff の想定モデル値 4.129 は実際には起こらないので、搬送波の
# 判定はこの床を張った値で行う。素のモデル値で判定すると必要以上に高い搬送波に張り付く。
#
# `0x19` は bit7 で PMD / AMD が切り替わるレジスタ。opm_seq.txt は @PMD@ → @AMD@ の
# 順に 2 回書くので、bit7=1 の側が PMD に、bit7=0 の側が AMD に入る。
AM = Mode("am",
          defines={"PMD": "00", "AMD": "7f", "AMSE": "80", "APMS": "02"},
          ladder=[(0x4A, 4), (0x52, 4), (0x4A, 8), (0x52, 8),
                  (0x4A, 15), (0x52, 15), (0x5A, 15)],
          cycles_min=0.70, tc_min=4.0, step_floor=8.0)

# はしごと閾値は AM の複製。PM 側だけ独立に変えてよい。
PM = Mode("pm",
          defines={"PMD": "ff", "AMD": "00", "AMSE": "00", "APMS": "70"},
          ladder=[(0x4A, 4), (0x52, 4), (0x4A, 8), (0x52, 8),
                  (0x4A, 15), (0x52, 15), (0x5A, 15)],
          cycles_min=0.70, tc_min=4.0, step_floor=8.0)

# 並び順がそのまま掃引順（モードが最外）になる
MODES = {m.name: m for m in (AM, PM)}

# --- 出来上がった .wav.zst の検査用 -------------------------------------

# opm-dac2wav.py が吐く WAV ヘッダ。`<4sL4s4sLHHLLHH4sL` の 44 バイト固定
WAV_HEADER = "<4sL4s4sLHHLLHH4sL"
WAV_HEADER_BYTES = struct.calcsize(WAV_HEADER)

# 実測長が要求のこの割合を下回っていたら短いと見なす。正常なら 0.9999 以上出る
CHECK_SHORT_RATIO = 0.98

# ファイル名から LFRQ を拾う（期待キャプチャ長はこれだけで決まる）
LFRQ_IN_NAME = re.compile(r"lfrq_([0-9a-f]{2})")


def lfo_step_samples(lfrq):
    """LFO 位相が 1 段進むのに掛かる想定サンプル数。"""
    inc = (16 + (lfrq & 0x0f)) << (lfrq >> 4)
    return LFO_STEP_COUNTS / inc


def lfo_step_ms(lfrq, sample_rate):
    """LFO 位相が 1 段進むのに掛かる想定時間 [ms]。"""
    return lfo_step_samples(lfrq) / sample_rate * 1000.0


def capture_ms(lfrq, sample_rate, steps, min_ms, max_ms):
    """LFRQ から `!capture` に渡すミリ秒を決める。"""
    want = steps * lfo_step_ms(lfrq, sample_rate)
    return int(round(min(max(want, min_ms), max_ms)))


def kc_carrier_hz(kc, phim, mul):
    """KC と MUL から搬送波（同相で重なった 4 オペレータの出力）の周波数 [Hz]。"""
    octave, note = (kc >> 4) & 7, kc & 0x0F
    n = (octave - (OPM_A4_KC >> 4)) * 12 + KC_SEMITONE[note] - KC_SEMITONE[OPM_A4_KC & 0x0F]
    return OPM_A4_HZ * (phim / OPM_A4_PHIM) * 2.0 ** (n / 12.0) * (mul if mul else 0.5)


def window_cond(period, tc):
    """段 period サンプルの Σt² の最悪／平均。

    1 に近いほど、段のゲインが搬送波位相に依らず決まる。搬送波の選択そのものには使わない
    （Mode の cycles_min のコメント参照）が、条件の見通しとして実行前に表示する。
    """
    d = 2.0 * math.pi / tc
    return max(0.0, 1.0 - abs(math.sin(period * d) / (period * math.sin(d))))


def carrier_locked(period, tc):
    """段が搬送波の半周期の整数倍に乗っているか（搬送波を選ぶときの除外条件）。

    tools/opm-lfo-period.py にも似た判定があるが、あちらは解析後に「求まった周期が
    搬送波の k/2 倍 (k<=16) に 4% 以内で近い」ことを警告するもので、別物。

    乗っていると、段の切れ目と搬送波の構造が区別できなくなる。ただし k=2（段 = 搬送波
    ちょうど 1 周期）は全段が同じ Σt² を持つ最良条件なので、k は 3 以上だけ見る。
    """
    return any(abs(period - k * tc / 2.0) <= 0.03 * k * tc / 2.0 for k in range(3, 7))


def choose_carrier(mode, lfrq, sample_rate, phim):
    """LFRQ から (KC, MUL) を選ぶ。段が判別できる範囲で最も低い周波数を採る。

    はしごも閾値もモードごとに持つ。判定に使う刻みは想定モデル値そのものではなく
    `mode.step_floor` で床を張った値。
    """
    period = max(lfo_step_samples(lfrq), mode.step_floor)
    for kc, mul in mode.ladder:
        tc = sample_rate / kc_carrier_hz(kc, phim, mul)
        if tc < mode.tc_min or period / tc < mode.cycles_min:
            continue
        if period < 4.0 * tc and carrier_locked(period, tc):
            continue
        return kc, mul
    return mode.ladder[-1]


# ---- 出来上がりの検査 ---------------------------------------------------

def wav_seconds(path):
    """.wav.zst の先頭 44 バイトだけ読んで中身の秒数を返す。

    読めなければ理由の文字列、長さ不定（WavSink の流し書き）なら None を返す。
    """
    try:
        with ZstdFile(path, "rb") as fp:
            head = fp.read(WAV_HEADER_BYTES)
    except (OSError, ValueError, EOFError, ZstdError) as e:
        # 中断で切れた .wav.zst は zstd フレームとして壊れている。フレーム自体が
        # 読めなければ ZstdError、44 バイト揃う前に切れていれば EOFError になる
        return f"読めない ({e})"
    if len(head) < WAV_HEADER_BYTES:
        return "WAV ヘッダが無い"

    riff, _, wave, _, _, _, _, rate, _, align, _, data, size = \
        struct.unpack(WAV_HEADER, head)
    if riff != b"RIFF" or wave != b"WAVE" or data != b"data":
        return "RIFF/WAVE ヘッダが無い"
    if not rate or not align:
        return "WAV ヘッダの値が不正"
    if size == 0xFFFFFFFF:
        return None                     # 長さ不定。検査できない
    return size / align / rate


def verify_capture(task):
    """撮れた .wav.zst が使い物になるかを見る。エラーなら理由の文字列。

    キャプチャが途中で死んでも sigrok-cli は終了コード 0 で終わるので、
    opm-writer.py の終了コードだけでは短いファイルを弾けない。掃引中はここで拾い、
    リトライに繋げる（--check の事後検査と同じ 98% を閾値にする）。
    """
    size = task.zst.stat().st_size if task.zst.exists() else 0
    if size == 0:
        return f"{task.zst} が生成されていない"

    if ZstdFile is None:
        return None                     # 3.13 以前。長さは検査できない

    want = task.time_ms / 1000.0
    got = wav_seconds(task.zst)
    if got is None:
        return None                     # 長さ不定の WAV。検査できない
    if isinstance(got, str):
        return got
    if got < want * CHECK_SHORT_RATIO:
        return (f"{got:.3f}s / 要求 {want:.3f}s "
                f"({100.0 * got / want:.1f}%) しか撮れていない")
    return None


def check_wav_dir(args, sample_rate):
    """--wav-dir 以下の .wav.zst が要求どおりの長さで撮れているかを見る。

    キャプチャが途中で死んでも sigrok-cli は終了コード 0 で終わるため、この検査を
    入れる前に撮ったファイルには短いものが混ざっている。撮り直しは --delete で
    消してから同じ掃引をもう一度回す。
    """
    if ZstdFile is None:
        print("! --check には Python 3.14 以降が要る（compression.zstd）",
              file=sys.stderr)
        return 1

    files = sorted(args.wav_dir.glob("*.wav.zst"))
    if not files:
        print(f"! {args.wav_dir} に .wav.zst が無い", file=sys.stderr)
        return 1

    short = []
    skipped = 0
    for path in files:
        m = LFRQ_IN_NAME.search(path.name)
        if m is None:
            print(f"? {path.name}: ファイル名から LFRQ を読めない", flush=True)
            skipped += 1
            continue
        want = capture_ms(int(m.group(1), 16), sample_rate,
                          args.steps, args.min_ms, args.max_ms) / 1000.0
        got = wav_seconds(path)
        if got is None:
            print(f"? {path.name}: 長さ不定の WAV なので検査できない", flush=True)
            skipped += 1
            continue
        if isinstance(got, str):
            print(f"! {path.name}: {got}", flush=True)
            short.append(path)
            continue
        if got < want * CHECK_SHORT_RATIO:
            print(f"! {path.name}: {got:.3f}s / 要求 {want:.3f}s "
                  f"({100.0 * got / want:.1f}%)", flush=True)
            short.append(path)

    if args.delete:
        for path in short:
            path.unlink()
        print(f"* 短い {len(short)} 件を消した。同じ掃引をもう一度回せば撮り直す",
              flush=True)

    tail = f" / 検査できず {skipped} 件" if skipped else ""
    print(f"--- 検査 {len(files)} 件 / 短い {len(short)} 件{tail}", flush=True)
    return 0 if not short else 1


# ---- 掃引の組み立て -----------------------------------------------------

class Task:
    """1 条件ぶんのキャプチャ。"""

    def __init__(self, mode, kc, mul, nfrq, lfrq, w, time_ms, wav_dir):
        self.mode = mode
        self.kc = kc
        self.mul = mul
        self.nfrq = nfrq
        self.lfrq = lfrq
        self.w = w
        self.time_ms = time_ms
        # 測定モードが先頭、掃引の主パラメータ (NFRQ / LFRQ) が次、搬送波の条件
        # (KC / MUL) が後。こうすると ls がモード → NFRQ → LFRQ の順に並び、
        # 同じ条件で搬送波だけ変えた追試が隣り合う。
        # W は既定の 3（ノイズ）のときだけ名前に出さない。W=3 で撮った既存の
        # データセット（wav/ wav_4a_01/）のファイル名をそのまま保つため
        wave = "" if w == DEFAULT_W else f"_w_{w:02x}"
        self.name = (f"{mode.name}{wave}_nfrq_{nfrq:02x}_lfrq_{lfrq:02x}"
                     f"_kc_{kc:02x}_mul_{mul:02x}")
        self.zst = wav_dir / f"{self.name}.wav.zst"


def build_tasks(args, sample_rate):
    # モードを最も外側にする。こうすると片方の測定のデータセットが先に完成し、
    # 途中で止めても 1 本は揃う。その次が KC で、先頭の KC のデータセットが
    # 先に完成し、残りの KC は追試として後から積み上がる。
    # LFRQ は降順。LFRQ が大きいほど LFO の周期が短く、キャプチャ時間も短いので、
    # 速く撮れる条件から先にファイルが揃う。
    #
    # KC / MUL を指定しなかった側は LFRQ ごとに choose_carrier が決める。両方
    # 指定すれば直積を掃く（同じ条件を別の搬送波で撮り直す追試用）。
    tasks = []
    for mode in args.mode:
        for kc in (args.kc or [None]):
            for mul in (args.mul or [None]):
                for nfrq in args.nfrq:
                    for lfrq in range(args.lfrq_end, args.lfrq_start - 1, -1):
                        auto_kc, auto_mul = choose_carrier(mode, lfrq, sample_rate,
                                                           args.phim)
                        ms = capture_ms(lfrq, sample_rate, args.steps,
                                        args.min_ms, args.max_ms)
                        tasks.append(Task(mode,
                                          kc if kc is not None else auto_kc,
                                          mul if mul is not None else auto_mul,
                                          nfrq, lfrq, args.w, ms, args.wav_dir))
    return tasks


def carrier_bands(tasks):
    """タスク列を (KC, MUL, LFRQ 下限, LFRQ 上限) の帯にまとめる。表示用。

    LFRQ が連続していて搬送波が同じあいだは 1 行にまとめる。搬送波はモードごとに
    選ぶので、渡すのは 1 モードぶんのタスク列。
    """
    bands = []
    for task in tasks:
        key = (task.kc, task.mul)
        if bands and bands[-1][0] == key and bands[-1][1] - 1 == task.lfrq:
            bands[-1][1] = task.lfrq
        elif not any(b[0] == key and b[1] <= task.lfrq <= b[2] for b in bands):
            bands.append([key, task.lfrq, task.lfrq])
    return [(kc, mul, lo, hi) for (kc, mul), lo, hi in bands]


def resume_index(tasks):
    """再開位置。.wav.zst が無い最初のタスクの 1 つ手前。

    直前の実行が途中で切れていると最後のファイルが不完全な可能性があるので、
    そこを撮り直すために 1 つ戻す。歯抜けがあっても最初の穴より先には進まない。
    全部揃っているときは最後の 1 件を上書き再撮する。
    """
    for i, task in enumerate(tasks):
        if not task.zst.exists():
            return max(i - 1, 0)
    return max(len(tasks) - 1, 0)


# ---- 各段の実行 ---------------------------------------------------------

def run(argv, timeout=None, echo_prefix=None):
    """サブプロセスを 1 個実行する。エラーなら理由の文字列。

    失敗時は出力を全部転記する。echo_prefix を渡すと成功時も、その文字で始まる行だけを
    転記する（dry-run でキャプチャのコマンドラインだけを見たいとき用）。
    """
    print(f"$ {shlex.join(str(a) for a in argv)}", flush=True)
    try:
        proc = subprocess.run([str(a) for a in argv], capture_output=True,
                              text=True, timeout=timeout)
    except FileNotFoundError:
        return f"{argv[0]} が見つからない"
    except subprocess.TimeoutExpired:
        return f"{Path(str(argv[0])).name} が {timeout:.0f} 秒で終わらなかった"

    lines = (proc.stdout + proc.stderr).splitlines()
    if proc.returncode != 0:
        for line in lines:
            print(f"| {line}", flush=True)
        return f"{Path(str(argv[0])).name} が終了コード {proc.returncode} で失敗した"
    if echo_prefix is not None:
        for line in lines:
            if line.startswith(echo_prefix):
                print(f"| {line}", flush=True)
    return None


def do_capture(task, args):
    """キャプチャ〜WAV 変換〜zstd 圧縮を opm-writer.py に一括でやらせる。

    出力名が `.wav.zst` なので、opm-writer.py 側が sigrok-cli とデコーダをパイプで繋ぐ。
    """
    argv = [sys.executable, OPM_WRITER, args.seq, task.zst,
            "-D", f"KC={task.kc:02x}",
            "-D", f"MUL={task.mul:02x}",
            "-D", f"NFRQ={task.nfrq:02x}",
            "-D", f"LFRQ={task.lfrq:02x}",
            "-D", f"W={task.w:02x}",
            "-D", f"TIME={task.time_ms}",
            "--phim", repr(args.phim),
            "--zstd-level", str(args.zstd_level),
            "--stop-on-error"]
    for key, val in task.mode.defines.items():
        argv += ["-D", f"{key}={val}"]
    if args.device:
        argv += ["--device", args.device]
    if args.samplerate:
        argv += ["--samplerate", args.samplerate]
    if args.dry_run:
        # dry-run でも opm-writer.py は -n で走らせる。中で組まれる
        # `sigrok-cli | opm-dac2wav.py` の行まで確認できる
        argv.append("-n")
        return run(argv, timeout=60.0, echo_prefix="$")
    # opm-writer.py 自身がパイプに「キャプチャ時間 + 120 秒」の制限を掛けるので、
    # こちらはさらに余裕を持たせる
    return run(argv, timeout=task.time_ms / 1000.0 + 300.0)


# ---- 表示 ---------------------------------------------------------------

def fmt_duration(sec):
    sec = int(round(sec))
    if sec >= 3600:
        return f"{sec // 3600}h{sec % 3600 // 60:02d}m"
    return f"{sec // 60}m{sec % 60:02d}s"


def fmt_size(n):
    if n >= 1024 * 1024:
        return f"{n / 1024 / 1024:.1f}MiB"
    return f"{n / 1024:.1f}KiB"


# ---- 実行ループ ---------------------------------------------------------

def execute(tasks, start, args):
    width = len(str(len(tasks)))
    t_begin = time.monotonic()
    overhead = []          # キャプチャ以外に掛かった実測時間
    total_zst = 0
    n_done = 0
    n_retry = 0

    for i in range(start, len(tasks)):
        task = tasks[i]
        remain_ms = sum(t.time_ms for t in tasks[i:])
        avg_overhead = sum(overhead) / len(overhead) if overhead else 8.0
        eta = remain_ms / 1000.0 + avg_overhead * (len(tasks) - i)
        print(f"[{i + 1:{width}d}/{len(tasks)}] {task.name}  "
              f"TIME={task.time_ms}ms  経過 {fmt_duration(time.monotonic() - t_begin)}  "
              f"残り約 {fmt_duration(eta)}", flush=True)

        # 失敗の実体はシリアルのタイムアウトや USB CDC の再列挙といった一過性の
        # 事象がほとんどなので、少し待って撮り直す。t0/t1 をループの内側で取るのは、
        # overhead に成功した試行のぶんだけを積んで ETA を狂わせないため
        for attempt in range(args.retry + 1):
            if attempt:
                # 短い .wav.zst が残っていると次の verify_capture が
                # 「生成されていない」を検出できなくなる
                task.zst.unlink(missing_ok=True)
                print(f"  リトライ {attempt}/{args.retry}"
                      f"（{args.retry_wait:.0f} 秒待つ）", flush=True)
                time.sleep(args.retry_wait)
                n_retry += 1
            t0 = time.monotonic()
            err = do_capture(task, args)
            if err is None and not args.dry_run:
                err = verify_capture(task)
            t1 = time.monotonic()
            if err is None:
                break
            print(f"! {task.name}: {err}", file=sys.stderr, flush=True)
        else:
            # 短い / 壊れたものを残すと --check が後から拾うことになる。消しておけば
            # 再開はここ（の 1 つ手前）から素直に撮り直す
            task.zst.unlink(missing_ok=True)
            print(f"! {i + 1} 件目を {args.retry + 1} 回試して撮れなかったので中断した。"
                  "同じコマンドで再実行すればここから再開する。",
                  file=sys.stderr, flush=True)
            return 1

        n_done += 1
        if args.dry_run:
            continue

        size = task.zst.stat().st_size
        total_zst += size
        overhead.append((t1 - t0) - task.time_ms / 1000.0)
        print(f"  cap {t1 - t0:.1f}s  zst {fmt_size(size)}", flush=True)

    tail = f" / リトライ {n_retry} 回" if n_retry else ""
    print(f"--- 完了 {n_done} 件 / 合計 {fmt_size(total_zst)} / "
          f"所要 {fmt_duration(time.monotonic() - t_begin)}{tail}", flush=True)
    return 0


# ---- エントリポイント ---------------------------------------------------

def parse_hex_list(value):
    out = []
    for tok in value.split(","):
        tok = tok.strip()
        if not tok:
            continue
        try:
            n = int(tok, 16)
        except ValueError:
            raise argparse.ArgumentTypeError(f"16 進数でない: {tok}")
        if not 0 <= n <= 0xff:
            raise argparse.ArgumentTypeError(f"00-ff の範囲外: {tok}")
        out.append(n)
    if not out:
        raise argparse.ArgumentTypeError("値が空")
    return out


def parse_hex(value):
    return parse_hex_list(value)[0]


def parse_kc_list(value):
    """KC の一覧。bit7 は未使用、下位ニブルの & 3 == 3 は無効な音名。"""
    out = parse_hex_list(value)
    for n in out:
        if n > 0x7F:
            raise argparse.ArgumentTypeError(f"KC は 00-7f: {n:02x}")
        if (n & 0x0F) not in KC_SEMITONE:
            raise argparse.ArgumentTypeError(
                f"KC の下位ニブルが無効な音名 (3,7,b,f は使えない): {n:02x}")
    return out


def parse_mul_list(value):
    """MUL の一覧。0 は 0.5 倍を意味する YM2151 の仕様どおりの値。"""
    out = []
    for tok in value.split(","):
        tok = tok.strip()
        if not tok:
            continue
        try:
            n = int(tok, 10)
        except ValueError:
            raise argparse.ArgumentTypeError(f"10 進数でない: {tok}")
        if not 0 <= n <= 15:
            raise argparse.ArgumentTypeError(f"MUL は 0-15: {tok}")
        out.append(n)
    if not out:
        raise argparse.ArgumentTypeError("値が空")
    return out


def parse_w(value):
    """LFO 波形。0=鋸 1=矩形 2=三角 3=ノイズ（レジスタ 0x1B の下位 2bit）。"""
    try:
        n = int(value, 10)
    except ValueError:
        raise argparse.ArgumentTypeError(f"10 進数でない: {value}")
    if not 0 <= n <= 3:
        raise argparse.ArgumentTypeError(f"W は 0-3: {value}")
    return n


def parse_mode_list(value):
    """測定モードの一覧。掃引はここに並べた順ではなく MODES の順で回る。"""
    out = []
    for tok in value.split(","):
        tok = tok.strip().lower()
        if not tok:
            continue
        if tok not in MODES:
            raise argparse.ArgumentTypeError(
                f"モードは {'/'.join(MODES)} のいずれか: {tok}")
        if MODES[tok] not in out:
            out.append(MODES[tok])
    if not out:
        raise argparse.ArgumentTypeError("値が空")
    return [m for m in MODES.values() if m in out]


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="YM2151 の LFO パラメータを掃引して AM / PM 測定のキャプチャを撮る")
    parser.add_argument("--seq", type=Path, default=SCRIPT_DIR / "opm_seq.txt",
                        help="シーケンスファイル（既定 %(default)s）")
    parser.add_argument("--mode", type=parse_mode_list,
                        default=list(MODES.values()), metavar="MODE[,MODE...]",
                        help=f"撮る測定モード（{'/'.join(MODES)}、既定は両方）")
    parser.add_argument("--wav-dir", type=Path, default=SCRIPT_DIR / "wav",
                        help="出力先。.wav.zst だけが置かれる（既定 %(default)s）")
    parser.add_argument("--kc", type=parse_kc_list, default=None,
                        metavar="HEX[,HEX...]",
                        help="振る KC。省略すると LFRQ ごとに自動選択する")
    parser.add_argument("--mul", type=parse_mul_list, default=None,
                        metavar="N[,N...]",
                        help="振る MUL（0-15、0 は 0.5 倍）。省略すると LFRQ ごとに"
                             "自動選択する")
    parser.add_argument("--nfrq", type=parse_hex_list, default=[0x00, 0x1f],
                        metavar="HEX[,HEX...]",
                        help="振る NFRQ（既定 00,1f）")
    parser.add_argument("--w", type=parse_w, default=DEFAULT_W, metavar="N",
                        help="LFO 波形（0=鋸 1=矩形 2=三角 3=ノイズ、既定 3）。"
                             "3 以外のときだけファイル名に _w_XX が入る")
    parser.add_argument("--lfrq-start", type=parse_hex, default=0x00, metavar="HEX",
                        help="LFRQ の下限（既定 00）")
    parser.add_argument("--lfrq-end", type=parse_hex, default=0xff, metavar="HEX",
                        help="LFRQ の上限（既定 ff）")
    parser.add_argument("--steps", type=int, default=32, metavar="N",
                        help="キャプチャしたい LFO の段数（既定 32）")
    parser.add_argument("--min-ms", type=int, default=500, metavar="MS",
                        help="キャプチャ長の下限（既定 500）")
    parser.add_argument("--max-ms", type=int, default=120000, metavar="MS",
                        help="キャプチャ長の上限（既定 120000）")
    parser.add_argument("--zstd-level", type=int, default=22, metavar="N",
                        help="zstd の圧縮レベル（既定 22 = 最大）")
    parser.add_argument("--retry", type=int, default=5, metavar="N",
                        help="1 条件あたりのリトライ回数（既定 5、0 で無効）")
    parser.add_argument("--retry-wait", type=float, default=5.0, metavar="SEC",
                        help="リトライまでの待ち時間 [秒]（既定 5）")
    parser.add_argument("--device", default=None,
                        help="USB CDC のデバイス（opm-writer.py へ素通し）")
    parser.add_argument("--samplerate", default=None,
                        help="sigrok-cli の samplerate（opm-writer.py へ素通し）")
    parser.add_argument("--phim", type=float, default=DEFAULT_PHIM,
                        help="OPM の φM [Hz]（既定 %(default)s）")
    parser.add_argument("--no-resume", action="store_true",
                        help="既存の .wav.zst を無視して先頭からやり直す")
    parser.add_argument("--check", action="store_true",
                        help="撮らずに、--wav-dir の .wav.zst が要求どおりの長さかを"
                             "検査する。短いものがあれば終了コード 1")
    parser.add_argument("--delete", action="store_true",
                        help="--check で見つかった短い .wav.zst を消す")
    parser.add_argument("-n", "--dry-run", action="store_true",
                        help="実行するコマンドラインを表示するだけ")
    args = parser.parse_args(argv)

    if args.lfrq_start > args.lfrq_end:
        print("! --lfrq-start が --lfrq-end より大きい", file=sys.stderr)
        return 1
    if args.steps <= 0 or args.min_ms <= 0 or args.max_ms < args.min_ms:
        print("! --steps / --min-ms / --max-ms の値が不正", file=sys.stderr)
        return 1
    if args.retry < 0 or args.retry_wait < 0:
        print("! --retry / --retry-wait に負の値は指定できない", file=sys.stderr)
        return 1
    if args.delete and not args.check:
        print("! --delete は --check と一緒に使う", file=sys.stderr)
        return 1

    if args.check:
        # 検査は既存ファイルだけを見るので、実機もデコーダも要らない
        return check_wav_dir(args, args.phim / 64.0)

    # デコーダは opm-writer.py がパイプで呼ぶ。ここで先に有無を見ておく
    for path in (args.seq, OPM_WRITER, OPM_DAC2WAV):
        if not path.exists():
            print(f"! 見つからない: {path}", file=sys.stderr)
            return 1

    sample_rate = args.phim / 64.0
    tasks = build_tasks(args, sample_rate)
    if not args.dry_run:
        args.wav_dir.mkdir(parents=True, exist_ok=True)

    start = 0 if args.no_resume else resume_index(tasks)
    total_ms = sum(t.time_ms for t in tasks[start:])

    print(f"# 測定 {','.join(m.name for m in args.mode)} / W {args.w} / "
          f"NFRQ {','.join(f'{n:02x}' for n in args.nfrq)} / "
          f"LFRQ {args.lfrq_end:02x}-{args.lfrq_start:02x}（降順）/ 計 {len(tasks)} 条件",
          flush=True)
    # 使う搬送波を LFRQ の帯ごとにまとめて並記する。「段/周期」が判別力を決める。
    # 搬送波はモードごとに選ぶので、帯もモードごとに出す
    for mode in args.mode:
        print(f"# [{mode.name}] {mode.define_str()}", flush=True)
        print("#   搬送波:  LFRQ 帯      KC/MUL   Hz  サンプル/周期  段/周期  cond",
              flush=True)
        for kc, mul, lo, hi in carrier_bands([t for t in tasks if t.mode is mode]):
            hz = kc_carrier_hz(kc, args.phim, mul)
            tc = sample_rate / hz
            step = max(lfo_step_samples(hi), mode.step_floor)
            rng = f"{hi:02x}" if lo == hi else f"{hi:02x}-{lo:02x}"
            print(f"#            {rng:<11s}  {kc:02x}/{mul:<5d} {hz:5.0f}  {tc:12.2f} "
                  f"{step / tc:8.2f}  {window_cond(step, tc):.2f}", flush=True)
    print(f"# fs={sample_rate:.0f}Hz  段数 {args.steps}  "
          f"キャプチャ長 {args.min_ms}-{args.max_ms}ms  リトライ {args.retry} 回  "
          f"出力 {args.wav_dir}", flush=True)
    if start > 0:
        print(f"# 再開: {start + 1} 件目 {tasks[start].name} を上書きするところから", flush=True)
    print(f"# 残り {len(tasks) - start} 件 / キャプチャ時間の合計 "
          f"{fmt_duration(total_ms / 1000.0)}", flush=True)

    try:
        return execute(tasks, start, args)
    except KeyboardInterrupt:
        print("\n! 中断された。同じコマンドで再実行すればここから再開する。",
              file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
