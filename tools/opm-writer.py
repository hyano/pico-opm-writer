#!/usr/bin/env python3
"""
pico-opm-writer にシーケンスファイルを流し込むホスト側スクリプト。

- 入力ファイルの各行を USB CDC 経由でファームへ送り、応答 (OK / ERR) を待つ
- `#` 以降を除去する。ファームは行頭 `#` の行しかコメントと見なさないので、
  行末コメントを含むシーケンスはこのスクリプト経由でしか流せない
- `@KEY@` を -D KEY=VALUE で置換する
- `!capture <ms>` で sigrok-cli によるロジックアナライザのキャプチャを行う

## キャプチャの出力形式は拡張子で決まる

    out.bin       sigrok-cli の生キャプチャをそのまま保存する（8MB/s で膨れる）
    out.wav       sigrok-cli | opm-dac2wav.py をパイプで繋いで WAV を書く
    out.wav.zst   同上。WAV を zstd で最大圧縮して書く

`.wav` / `.wav.zst` では生キャプチャも中間の WAV も**ディスクに落とさない**。
10 秒のキャプチャが 80MB の .bin + 2.5MB の .wav ではなく数百 KB の .wav.zst だけになる。
デコードがその場で走るので、`--phim` などを間違えると取り直しになる点だけ注意する
（デバッグしたいときは `.bin` でキャプチャする）。

外部ライブラリは使わない（標準ライブラリのみ）。

SPDX-License-Identifier: MIT
"""

import argparse
import collections
import errno
import os
import re
import select
import shlex
import subprocess
import sys
import termios
import time
from pathlib import Path

# ---- 設定 ---------------------------------------------------------------

# pico-opm-writer 自身の USB CDC（PicoProbe 側の CDC-UART ではない）
SERIAL_DEVICE = "/dev/cu.usbmodem112101"

# デバイスノードが現れるまでの待ち時間（SWD reset 直後は数秒消える）
SERIAL_WAIT_S = 10.0

# 応答 1 行あたりの基本タイムアウト。`d <ms>` の待ち時間はこれに加算する
REPLY_TIMEOUT_S = 5.0

# 接続直後の起動バナーを読み捨てる時間
BANNER_DRAIN_S = 1.0

# sigrok-cli の samplerate
SIGROK_SAMPLERATE = "8m"

# sigrok-cli のコマンドライン。{samplerate} / {time_ms} が実行時に埋まる。
# `-o` を付けなければ標準出力へ書く（sigrok-cli(1) の --output-file）
SIGROK_ARGV = [
    "sigrok-cli",
    "-d", "fx2lafw",
    "--config", "samplerate={samplerate}",
    "-O", "binary",
    "--time={time_ms}",
]

# ファイルへ落とすときだけ足す。{output} が実行時に埋まる
SIGROK_OUTPUT_ARGV = ["-o", "{output}"]

# sigrok-cli のプロセスを打ち切るまでのマージン（キャプチャ時間に加算）
SIGROK_TIMEOUT_MARGIN_S = 30.0

# パイプ経路のマージン。キャプチャ後にデコードと zstd の最終フラッシュが少し残る
PIPELINE_TIMEOUT_MARGIN_S = 120.0

# sigrok-cli の出力をデコーダへ中継するときの読み単位
PUMP_READ_BYTES = 1 << 20

# デコーダが遅れたときに親が抱えるバックログの上限。超えたら失敗にする
PUMP_BACKLOG_MAX = 256 << 20

# 届いたキャプチャが要求のこの割合を下回ったら失敗扱いにする
CAPTURE_SHORT_RATIO = 0.99

# パイプで繋ぐデコーダ。自分と同じディレクトリに置かれている前提
DAC2WAV = Path(__file__).resolve().parent / "opm-dac2wav.py"

# デコーダへ渡す既定値
DEFAULT_PHIM = 4000000.0
DEFAULT_ZSTD_LEVEL = 22

# 出力の拡張子 → キャプチャの形式。長い方から見る
CAPTURE_SUFFIXES = (".wav.zst", ".wav")

# sigrok の samplerate 表記の接尾辞
SAMPLERATE_SUFFIX = {"k": 1e3, "m": 1e6, "g": 1e9}

# ------------------------------------------------------------------------

KEYWORD_RE = re.compile(r"@([A-Za-z0-9_]+)@")


# ---- 前処理・検証 -------------------------------------------------------

class Step:
    """実行 1 ステップ。kind は "line"（ファームへ送る）か "capture"。"""

    def __init__(self, kind, lineno, text, ms=None, output=None, fmt=None):
        self.kind = kind
        self.lineno = lineno
        self.text = text
        self.ms = ms
        self.output = output
        self.fmt = fmt      # capture のときの出力形式（"bin" / "wav" / "wav.zst"）


def substitute(text, defines, undefined):
    """`@KEY@` を置換する。未定義キーは undefined に集めて元のまま残す。"""

    def repl(m):
        key = m.group(1)
        if key in defines:
            return defines[key]
        undefined.append(key)
        return m.group(0)

    return KEYWORD_RE.sub(repl, text)


def capture_kind(base):
    """出力名の拡張子からキャプチャの形式を決める。不明な拡張子は生キャプチャ。"""
    name = base.name.lower()
    for suffix in CAPTURE_SUFFIXES:
        if name.endswith(suffix):
            return suffix[1:]       # "wav.zst" / "wav"
    return "bin"


def capture_path(base, index):
    """1 個目は指定名のまま、2 個目以降は拡張子の手前に -N を付ける。"""
    if index == 1:
        return base
    kind = capture_kind(base)
    # `.wav.zst` は 1 つの拡張子として扱う（`out.wav-2.zst` にならないように）
    n = len(base.suffix) if kind == "bin" else len(kind) + 1
    stem, tail = (base.name[:-n], base.name[-n:]) if n else (base.name, "")
    return base.with_name(f"{stem}-{index}{tail}")


def parse_samplerate(text):
    """sigrok の samplerate 表記（`8m` / `16MHz` / `8000000`）を Hz にする。

    デコーダの `--la-rate` へ渡すためだけに使う。読めなければ None（既定値に任せる）。
    """
    s = text.strip().lower().removesuffix("hz").strip()
    mul = 1.0
    if s and s[-1] in SAMPLERATE_SUFFIX:
        mul, s = SAMPLERATE_SUFFIX[s[-1]], s[:-1].strip()
    try:
        return float(s) * mul
    except ValueError:
        return None


def preprocess(path, defines, capture_base):
    """入力ファイルを Step のリストへ変換する。戻り値は (steps, errors)。"""
    steps = []
    errors = []
    n_capture = 0

    with open(path, "r", encoding="utf-8") as f:
        raw_lines = f.readlines()

    for lineno, raw in enumerate(raw_lines, start=1):
        # 行末コメントを落としてから置換する（コメント内のキーは無視される）
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue

        undefined = []
        line = substitute(line, defines, undefined)
        for key in undefined:
            errors.append(f"{path}:{lineno}: 未定義のキーワード @{key}@")
        if undefined:
            continue

        if not line.startswith("!"):
            steps.append(Step("line", lineno, line))
            continue

        # 特殊コマンド
        tokens = line[1:].split()
        name = tokens[0].lower() if tokens else ""
        args = tokens[1:]

        if name != "capture":
            errors.append(f"{path}:{lineno}: 未知の特殊コマンド !{name or ''}")
            continue
        if len(args) != 1:
            errors.append(f"{path}:{lineno}: !capture は引数を 1 個（ms）取る: {line}")
            continue
        if not args[0].isdigit() or int(args[0]) <= 0:
            errors.append(f"{path}:{lineno}: !capture の引数が正の 10 進整数でない: {args[0]}")
            continue

        n_capture += 1
        output = capture_path(capture_base, n_capture)
        steps.append(Step("capture", lineno, line,
                          ms=int(args[0]),
                          output=output,
                          fmt=capture_kind(output)))

    return steps, errors


# ---- シリアル -----------------------------------------------------------

class Serial:
    """USB CDC を raw モードで開く最小限のラッパ。"""

    def __init__(self, device, wait_s):
        deadline = time.monotonic() + wait_s
        while not os.path.exists(device):
            if time.monotonic() >= deadline:
                raise RuntimeError(f"シリアルデバイスが現れない: {device}")
            time.sleep(0.2)

        try:
            self.fd = os.open(device, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        except OSError as e:
            if e.errno == errno.EBUSY:
                raise RuntimeError(
                    f"{device} は他のプロセスが使用中"
                    "（screen / minicom などを終了すること）") from e
            raise RuntimeError(f"{device} を開けない: {e}") from e

        try:
            self.saved = termios.tcgetattr(self.fd)
            self._set_raw()
        except Exception:
            os.close(self.fd)
            raise

        self.buf = b""

    def _set_raw(self):
        iflag, oflag, cflag, lflag, ispeed, ospeed, cc = termios.tcgetattr(self.fd)
        iflag &= ~(termios.IGNBRK | termios.BRKINT | termios.PARMRK | termios.ISTRIP |
                   termios.INLCR | termios.IGNCR | termios.ICRNL | termios.IXON)
        oflag &= ~termios.OPOST
        lflag &= ~(termios.ECHO | termios.ECHONL | termios.ICANON |
                   termios.ISIG | termios.IEXTEN)
        cflag &= ~(termios.CSIZE | termios.PARENB | termios.CRTSCTS)
        cflag |= termios.CS8 | termios.CREAD | termios.CLOCAL
        cc = list(cc)
        cc[termios.VMIN] = 0
        cc[termios.VTIME] = 0
        # CDC ではボーレートは無視されるが、一応妥当な値を入れておく
        speed = termios.B115200
        termios.tcsetattr(self.fd, termios.TCSANOW,
                          [iflag, oflag, cflag, lflag, speed, speed, cc])

    def close(self):
        try:
            termios.tcsetattr(self.fd, termios.TCSANOW, self.saved)
        except Exception:
            pass
        os.close(self.fd)

    def write_line(self, line):
        data = (line + "\n").encode("utf-8")
        while data:
            if not select.select([], [self.fd], [], 5.0)[1]:
                raise RuntimeError("シリアルへの書き込みがタイムアウトした")
            data = data[os.write(self.fd, data):]

    def read_line(self, deadline):
        """1 行読む。行が揃わないままタイムアウトしたら None。"""
        while True:
            nl = self.buf.find(b"\n")
            if nl >= 0:
                line, self.buf = self.buf[:nl], self.buf[nl + 1:]
                return line.rstrip(b"\r").decode("utf-8", "replace")

            remain = deadline - time.monotonic()
            if remain <= 0:
                return None
            if not select.select([self.fd], [], [], remain)[0]:
                return None
            try:
                chunk = os.read(self.fd, 4096)
            except BlockingIOError:
                continue
            if chunk:
                self.buf += chunk

    def drain(self, seconds):
        """指定時間ぶん読み捨てる（受け取った行を返す）。"""
        deadline = time.monotonic() + seconds
        lines = []
        while True:
            line = self.read_line(deadline)
            if line is None:
                return lines
            lines.append(line)


# ---- 実行 ---------------------------------------------------------------

def reply_timeout(line):
    """コマンドに応じた応答待ち時間。`d <ms>` と r/c は余分に待つ。"""
    tokens = line.split()
    cmd = tokens[0].lower() if tokens else ""
    if cmd == "d" and len(tokens) >= 2 and tokens[1].isdigit():
        return REPLY_TIMEOUT_S + int(tokens[1]) / 1000.0
    if cmd in ("r", "c"):
        return REPLY_TIMEOUT_S + 5.0
    return REPLY_TIMEOUT_S


def send_line(ser, line):
    """1 行送って終端応答まで読む。エラーなら理由の文字列、正常なら None。"""
    print(f"> {line}", flush=True)
    ser.write_line(line)

    deadline = time.monotonic() + reply_timeout(line)
    while True:
        reply = ser.read_line(deadline)
        if reply is None:
            print("! 応答がタイムアウトした", file=sys.stderr, flush=True)
            return "timeout"
        print(f"< {reply}", flush=True)
        if reply.startswith("#"):
            continue  # 情報行。終端はこの後に来る
        if reply == "OK":
            return None
        return reply


def sigrok_argv(step, args, to_stdout):
    """sigrok-cli のコマンドライン。to_stdout なら `-o` を付けない。"""
    argv = list(SIGROK_ARGV)
    if not to_stdout:
        argv += SIGROK_OUTPUT_ARGV
    return [a.format(samplerate=args.samplerate, time_ms=step.ms,
                     output=str(step.output)) for a in argv]


def dac2wav_argv(step, args):
    """パイプで受ける opm-dac2wav.py のコマンドライン。"""
    argv = [sys.executable, str(DAC2WAV), "-", str(step.output),
            "--phim", repr(args.phim),
            "--zstd-level", str(args.zstd_level)]
    la_rate = parse_samplerate(args.samplerate)
    if la_rate:
        argv += ["--la-rate", repr(la_rate)]
    # .bin が残らないので、デコーダの統計が唯一の健全性チェックになる
    argv.append("-v")
    return argv


def capture_command(step, args):
    """表示用のコマンドライン文字列。"""
    if step.fmt == "bin":
        return shlex.join(sigrok_argv(step, args, to_stdout=False))
    return (shlex.join(sigrok_argv(step, args, to_stdout=True)) + " | " +
            shlex.join(dac2wav_argv(step, args)))


def expected_bytes(step, args):
    """要求どおりキャプチャできた場合に sigrok-cli が吐くバイト数。読めなければ None。

    `-O binary` は 1 サンプル 1 バイトなので samplerate × 時間そのもの。
    """
    rate = parse_samplerate(args.samplerate)
    return int(rate * step.ms / 1000.0) if rate else None


def report_output(step, args, captured):
    """キャプチャ長を検査して結果を表示する。エラーなら理由の文字列。

    captured は sigrok-cli が実際に吐いたバイト数。途中で取得が死んでも sigrok-cli は
    終了コード 0 で終わるため、長さを見ないと短い出力を成功と誤認する。
    """
    if not step.output.exists():
        return f"キャプチャファイルが生成されていない: {step.output}"

    want = expected_bytes(step, args)
    rate = parse_samplerate(args.samplerate)
    if want is not None and captured < want * CAPTURE_SHORT_RATIO:
        return discard_output(
            step, f"キャプチャが短い: {captured / rate:.3f}s / "
                  f"要求 {step.ms / 1000.0:.3f}s ({100.0 * captured / want:.1f}%)")

    tail = f" / キャプチャ {captured / rate:.3f}s" if rate else ""
    print(f"* {step.output} ({step.output.stat().st_size} bytes){tail}", flush=True)
    return None


def run_capture(step, args):
    """`!capture` を実行する。エラーなら理由の文字列。"""
    print(f"$ {capture_command(step, args)}", flush=True)
    step.output.parent.mkdir(parents=True, exist_ok=True)
    if step.fmt == "bin":
        return run_capture_raw(step, args)
    return run_capture_pipeline(step, args)


def run_capture_raw(step, args):
    """sigrok-cli を起動し、生キャプチャをファイルへ落とす。"""
    argv = sigrok_argv(step, args, to_stdout=False)
    timeout = step.ms / 1000.0 + SIGROK_TIMEOUT_MARGIN_S
    try:
        proc = subprocess.run(argv, capture_output=True, text=True, timeout=timeout)
    except FileNotFoundError:
        return f"{argv[0]} が見つからない"
    except subprocess.TimeoutExpired:
        return f"sigrok-cli が {timeout:.0f} 秒で終わらなかった"

    for stream in (proc.stdout, proc.stderr):
        for out in stream.splitlines():
            print(f"| {out}", flush=True)

    if proc.returncode != 0:
        return f"sigrok-cli が終了コード {proc.returncode} で失敗した"
    size = step.output.stat().st_size if step.output.exists() else 0
    return report_output(step, args, size)


def run_capture_pipeline(step, args):
    """sigrok-cli → 親 → opm-dac2wav.py を組み、キャプチャと変換を同時に走らせる。

    生キャプチャは 8MB/s で膨れるのでディスクへは落とさない。書くのは最終出力だけ。
    **両者を直結せず、データは親が中継する**（pump_pipeline のコメントを参照）。
    """
    sig_argv = sigrok_argv(step, args, to_stdout=True)
    dec_argv = dac2wav_argv(step, args)
    timeout = step.ms / 1000.0 + PIPELINE_TIMEOUT_MARGIN_S

    try:
        sig = subprocess.Popen(sig_argv, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE)
    except FileNotFoundError:
        return f"{sig_argv[0]} が見つからない"

    try:
        dec = subprocess.Popen(dec_argv, stdin=subprocess.PIPE,
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    except OSError as e:
        sig.stdout.close()
        sig.kill()
        sig.wait()
        return f"{DAC2WAV.name} を起動できない: {e}"

    try:
        ok, captured, overflow = pump_pipeline(sig, dec, timeout)
    finally:
        for fp in (sig.stdout, dec.stdin):
            try:
                fp.close()
            except OSError:
                pass

    if overflow:
        for proc in (dec, sig):
            proc.kill()
            proc.wait()
        return discard_output(
            step, f"デコーダが追いつかず、未処理データが {PUMP_BACKLOG_MAX} バイトを超えた")
    if not ok:
        for proc in (dec, sig):
            proc.kill()
            proc.wait()
        return discard_output(step, f"キャプチャが {timeout:.0f} 秒で終わらなかった")

    # デコーダを先に見る（デコーダが落ちると sigrok は EPIPE で二次的に失敗するので、
    # そちらのコードを出しても原因が分からない）
    if dec.returncode != 0:
        return discard_output(
            step, f"{DAC2WAV.name} が終了コード {dec.returncode} で失敗した")
    if sig.returncode != 0:
        return discard_output(
            step, f"sigrok-cli が終了コード {sig.returncode} で失敗した")
    return report_output(step, args, captured)


def discard_output(step, err):
    """失敗した回の出力を消す。長さの足りない WAV がデータセットに混ざる方が厄介。"""
    if step.output.exists():
        step.output.unlink()
        print(f"* 失敗したので {step.output} は消した", flush=True)
    return err


def echo_stderr(fds, fd):
    """子プロセスの stderr を 1 回読み、行が揃うたびに `| ` 付きで出す。

    EOF に達したらその fd を fds から外す（= そのプロセスは終了間際）。
    """
    fp, buf = fds[fd]
    try:
        chunk = os.read(fd, 4096)
    except BlockingIOError:
        return
    if not chunk:
        if buf:
            print(f"| {buf.decode('utf-8', 'replace')}", flush=True)
        fp.close()
        del fds[fd]
        return
    buf += chunk
    while True:
        nl = buf.find(b"\n")
        if nl < 0:
            break
        line = bytes(buf[:nl]).decode("utf-8", "replace").rstrip("\r")
        del buf[:nl + 1]
        print(f"| {line}", flush=True)


def pump_pipeline(sig, dec, timeout):
    """sigrok の出力をデコーダへ中継しつつ、両者の stderr を流して終了を待つ。

    **sigrok とデコーダを直結してはいけない。** デコーダが 1 チャンクを処理している間
    パイプは読まれず、OS のパイプ容量（64KiB = 8MB/s で 8ms ぶん）を超えると
    sigrok-cli の write が待たされる。その間 sigrok-cli は libusb の転送を回せないので
    ロジアナ側の FIFO が溢れ、取得が途中で死ぬ。しかも sigrok-cli は終了コード 0 で
    終わるため、短いキャプチャが黙って通ってしまう。

    親はキャプチャ中ほかに何もしていないので、ここで中継役に回る。デコーダが一時的に
    遅れたぶんは RAM に抱え、sigrok 側は決して待たせない。

    戻り値は (時間内に終わったか, デコーダへ渡したバイト数, バックログが溢れたか)。
    """
    data_fd = sig.stdout.fileno()
    sink_fd = dec.stdin.fileno()
    os.set_blocking(data_fd, False)
    os.set_blocking(sink_fd, False)

    fds = {proc.stderr.fileno(): (proc.stderr, bytearray()) for proc in (sig, dec)}
    backlog = collections.deque()   # まだデコーダへ渡していない memoryview の列
    pending = 0                     # backlog の合計バイト数
    forwarded = 0
    data_open = True                # sigrok の出力がまだ続くか
    sink_open = True                # デコーダの stdin がまだ開いているか
    deadline = time.monotonic() + timeout

    while fds or data_open or pending:
        remain = deadline - time.monotonic()
        if remain <= 0:
            return False, forwarded, False

        rlist = list(fds)
        # バックログが上限に達したら読むのを止める（そこで初めて sigrok を待たせる）
        if data_open and pending < PUMP_BACKLOG_MAX:
            rlist.append(data_fd)
        wlist = [sink_fd] if sink_open and pending else []
        ready_r, ready_w, _ = select.select(rlist, wlist, [], remain)
        if not ready_r and not ready_w:
            return False, forwarded, False

        if data_fd in ready_r:
            try:
                chunk = os.read(data_fd, PUMP_READ_BYTES)
            except BlockingIOError:
                chunk = None            # まだ来ていないだけ
            if chunk:
                backlog.append(memoryview(chunk))
                pending += len(chunk)
            elif chunk is not None:     # b"" = sigrok が出力を閉じた
                data_open = False

        for fd in list(ready_r):
            if fd in fds:
                echo_stderr(fds, fd)

        if sink_fd in ready_w and backlog:
            head = backlog[0]
            try:
                n = os.write(sink_fd, head)
            except BlockingIOError:
                n = 0
            except OSError:
                # デコーダが落ちた。読み口も閉じて sigrok を EPIPE で終わらせる
                backlog.clear()
                pending = 0
                sink_open = False
                data_open = False
                sig.stdout.close()
                continue
            if n:
                pending -= n
                forwarded += n
                backlog[0] = head[n:]
                if not len(backlog[0]):
                    backlog.popleft()

        if sink_open and not data_open and not pending:
            dec.stdin.close()       # これがデコーダへの EOF になる
            sink_open = False

        if pending >= PUMP_BACKLOG_MAX:
            return False, forwarded, True

    for proc in (sig, dec):
        try:
            proc.wait(timeout=max(1.0, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            return False, forwarded, False
    return True, forwarded, False


def execute(steps, args):
    ser = None
    n_sent = 0
    n_capture = 0
    n_error = 0

    try:
        if not args.dry_run:
            ser = Serial(args.device, SERIAL_WAIT_S)
            for line in ser.drain(BANNER_DRAIN_S):
                print(f"< {line}", flush=True)

        for step in steps:
            if step.kind == "line":
                n_sent += 1
                if args.dry_run:
                    print(f"> {step.text}", flush=True)
                    continue
                err = send_line(ser, step.text)
            else:
                n_capture += 1
                if args.dry_run:
                    print(f"$ {capture_command(step, args)}", flush=True)
                    continue
                err = run_capture(step, args)

            if err is not None:
                n_error += 1
                print(f"! {args.input}:{step.lineno}: {step.text}: {err}",
                      file=sys.stderr, flush=True)
                if args.stop_on_error:
                    break
    finally:
        if ser is not None:
            ser.close()

    print(f"--- 送信 {n_sent} 行 / キャプチャ {n_capture} 件 / エラー {n_error} 件",
          flush=True)
    return 0 if n_error == 0 else 1


# ---- エントリポイント ---------------------------------------------------

def parse_define(value):
    if "=" not in value:
        raise argparse.ArgumentTypeError(f"KEY=VALUE の形式でない: {value}")
    key, val = value.split("=", 1)
    if not KEYWORD_RE.fullmatch(f"@{key}@"):
        raise argparse.ArgumentTypeError(f"キーに使えない文字が含まれる: {key}")
    return key, val


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="pico-opm-writer にシーケンスファイルを流し込む")
    parser.add_argument("input", type=Path, help="入力シーケンスファイル")
    parser.add_argument("capture", type=Path,
                        help="キャプチャの出力ファイル名。拡張子で形式が決まる"
                             "（.wav / .wav.zst はパイプで WAV へ変換、他は生キャプチャ）")
    parser.add_argument("-D", "--define", type=parse_define, action="append",
                        default=[], metavar="KEY=VALUE",
                        help="@KEY@ の置換値（複数指定可）")
    parser.add_argument("--device", default=SERIAL_DEVICE,
                        help=f"USB CDC のデバイス（既定 {SERIAL_DEVICE}）")
    parser.add_argument("--samplerate", default=SIGROK_SAMPLERATE,
                        help=f"sigrok-cli の samplerate（既定 {SIGROK_SAMPLERATE}）")
    parser.add_argument("--phim", type=float, default=DEFAULT_PHIM,
                        help="OPM の φM [Hz]（.wav / .wav.zst のとき"
                             f"デコーダへ渡す。既定 {DEFAULT_PHIM:g}）")
    parser.add_argument("--zstd-level", type=int, default=DEFAULT_ZSTD_LEVEL,
                        metavar="N",
                        help=f".wav.zst の圧縮レベル（既定 {DEFAULT_ZSTD_LEVEL} = 最大）")
    parser.add_argument("-n", "--dry-run", action="store_true",
                        help="シリアルも sigrok も触らず、実行内容だけ表示する")
    parser.add_argument("--stop-on-error", action="store_true",
                        help="エラーが出た時点で中断する（既定は警告して続行）")
    args = parser.parse_args(argv)

    defines = dict(args.define)

    try:
        steps, errors = preprocess(args.input, defines, args.capture)
    except OSError as e:
        print(f"! {e}", file=sys.stderr)
        return 1

    if errors:
        for e in errors:
            print(f"! {e}", file=sys.stderr)
        return 1

    if any(s.kind == "capture" and s.fmt != "bin" for s in steps) \
            and not DAC2WAV.exists():
        print(f"! デコーダが見つからない: {DAC2WAV}", file=sys.stderr)
        return 1

    try:
        return execute(steps, args)
    except KeyboardInterrupt:
        print("\n! 中断された", file=sys.stderr)
        return 1
    except RuntimeError as e:
        print(f"! {e}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
