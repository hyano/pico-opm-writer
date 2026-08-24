#!/usr/bin/env python3
"""
pico-opm-writer にシーケンスファイルを流し込むホスト側スクリプト。

- 入力ファイルの各行を USB CDC 経由でファームへ送り、応答 (OK / ERR) を待つ
- `#` 以降を除去する。ファームは行頭 `#` の行しかコメントと見なさないので、
  行末コメントを含むシーケンスはこのスクリプト経由でしか流せない
- `@KEY@` を -D KEY=VALUE で置換する
- `!capture <ms>` でファームの CDC #1 から PCM をキャプチャし WAV へ変換する

## キャプチャの出力形式は拡張子で決まる

    out.wav       ファームの CDC #1 から読んだ PCM を WAV に書く
    out.wav.zst   同上。WAV を zstd で最大圧縮して書く

この 2 つ以外の拡張子は受け付けない。WAV に書くサンプリングレートは φM/64 なので、
`--phim` は実機の φM と合わせること（ずれると時間軸のずれた WAV が黙って出来上がる）。

外部ライブラリは使わない（標準ライブラリのみ）。

SPDX-License-Identifier: MIT
"""

import argparse
import errno
import os
import plistlib
import re
import select
import struct
import subprocess
import sys
import termios
import time
from pathlib import Path

try:                                    # Python 3.14+ (PEP 784)
    from compression.zstd import CompressionParameter, ZstdFile
except ImportError:                     # 3.13 以前は zstd 出力を諦める
    ZstdFile = None

# ---- 設定 ---------------------------------------------------------------

# ファーム側 usb_descriptors.c が CDC #0 / #1 に付けている USB インタフェース名。
# tty 名（/dev/cu.usbmodem<location>0N）は USB のポート位置で変わるので、
# 決め打ちにせずこの名前から IORegistry を引いてデバイスノードを求める。
CDC_COMMAND_NAME = "pico-opm-writer command"
CDC_PCM_NAME = "pico-opm-writer PCM"

# PCM 1 フレーム（L と R の int16）のバイト数
PCM_FRAME_BYTES = 4

# `p 0` を送ってから応答が返るまでの上限。ファーム側は 2 秒で打ち切る
PCM_DRAIN_TIMEOUT_S = 5.0

# `p 0` の応答後、ホスト側に残っている分を拾い切るまでの無音待ち
PCM_TAIL_QUIET_S = 0.3

# PCM ポートの 1 回の読み単位
PCM_READ_BYTES = 1 << 16

# デバイスノードが現れるまでの待ち時間（SWD reset 直後は数秒消える）
SERIAL_WAIT_S = 10.0

# 応答 1 行あたりの基本タイムアウト。`d <ms>` の待ち時間はこれに加算する
REPLY_TIMEOUT_S = 5.0

# 接続直後の起動バナーを読み捨てる時間
BANNER_DRAIN_S = 1.0

# 届いたキャプチャが要求のこの割合を下回ったら失敗扱いにする
CAPTURE_SHORT_RATIO = 0.99

# WAV へ書く既定値
DEFAULT_PHIM = 4000000.0
DEFAULT_ZSTD_LEVEL = 22

# 出力の拡張子 → キャプチャの形式。長い方から見る
CAPTURE_SUFFIXES = (".wav.zst", ".wav")

# WavSink がヘッダのサイズ欄を確定させるために溜めておける PCM の上限 [byte]。
# 64MiB = 62500Hz / 16bit / stereo で約 17 分ぶん。
WAV_BUFFER_MAX = 64 << 20

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
        self.fmt = fmt      # capture のときの出力形式（"wav" / "wav.zst"）


# ---- WAV 出力 -----------------------------------------------------------

class WavSink:
    """16bit PCM の WAV を書き出す。`wave` の代わりに使う最小限の実装。

    `wave` はヘッダのサイズ欄を close() 時に書き戻すためシーカブルな出力を要求するが、
    こちらはパイプや zstd ストリームにも書けるようにする必要がある。そのため PCM を
    WAV_BUFFER_MAX まではメモリに溜め、close() でサイズ欄を確定させてから一気に書く。
    溜めきれなくなったらサイズ欄を 0xFFFFFFFF にしたヘッダを先に吐いて素通しへ切り替え、
    シーク可能な出力ならそのヘッダを close() で書き戻す。

    ヘッダのバイト並びは `wave` が出すものと同じ（44 バイト、RIFF/fmt /data）。
    """

    def __init__(self, fp, nchannels, sampwidth, framerate,
                 buffer_max=WAV_BUFFER_MAX):
        self.fp = fp
        self.nchannels = nchannels
        self.sampwidth = sampwidth
        self.framerate = framerate
        self.buffer_max = buffer_max
        self.chunks = []        # まだ書き出していない PCM（素通しに入る前だけ使う）
        self.nbytes = 0
        self.streaming = False  # ヘッダを先に吐いてしまったか
        self.header_at = None   # 書き戻すためのヘッダ位置。シーク不可なら None

    def _header(self, datalength):
        """44 バイトのヘッダ。datalength=None ならサイズ欄を不定 (0xFFFFFFFF) にする。"""
        if datalength is None:
            riff = data = 0xFFFFFFFF
        else:
            riff, data = 36 + datalength, datalength
        return struct.pack("<4sL4s4sLHHLLHH4sL",
                           b"RIFF", riff, b"WAVE",
                           b"fmt ", 16, 1, self.nchannels, self.framerate,
                           self.nchannels * self.framerate * self.sampwidth,
                           self.nchannels * self.sampwidth, self.sampwidth * 8,
                           b"data", data)

    def writeframes(self, data):
        self.nbytes += len(data)
        if self.streaming:
            self.fp.write(data)
            return
        self.chunks.append(data)
        if self.nbytes > self.buffer_max:
            self._start_streaming()

    def _start_streaming(self):
        try:
            self.header_at = self.fp.tell() if self.fp.seekable() else None
        except (AttributeError, OSError):
            self.header_at = None
        self._flush(None)
        self.streaming = True
        if self.header_at is None:
            print(f"! WAV が {self.buffer_max} バイトを超えたので、ヘッダのサイズ欄を "
                  "0xFFFFFFFF にして流し書きする（長さ不定の WAV になる）",
                  file=sys.stderr)

    def _flush(self, datalength):
        self.fp.write(self._header(datalength))
        for chunk in self.chunks:
            self.fp.write(chunk)
        self.chunks = []

    def close(self):
        if not self.streaming:
            self._flush(self.nbytes)
        elif self.header_at is not None:
            # 素通しに入ったがシークできる出力だった。サイズ欄を実測値で上書きする
            self.fp.seek(self.header_at)
            self.fp.write(self._header(self.nbytes))


def open_output(path, level):
    """出力を開いて (fp, closers) を返す。`.zst` なら zstd で圧縮する。

    `closers` は呼び出し側が逆順に閉じる。
    """
    compress = str(path).endswith(".zst")
    if compress and ZstdFile is None:
        # ファイルを作る前に弾く（空の出力を残さない）
        raise OSError("zstd 圧縮の出力には Python 3.14 以降が必要")

    raw = open(path, "wb")
    closers = [raw]

    fp = raw
    if compress:
        fp = ZstdFile(raw, "wb",
                      options={CompressionParameter.compression_level: level})
        closers.append(fp)
    return fp, closers


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
    """出力名の拡張子からキャプチャの形式を決める。.wav / .wav.zst のみ対応。"""
    name = base.name.lower()
    for suffix in CAPTURE_SUFFIXES:
        if name.endswith(suffix):
            return suffix[1:]       # "wav.zst" / "wav"
    return None


def capture_path(base, index):
    """1 個目は指定名のまま、2 個目以降は拡張子の手前に -N を付ける。

    呼ぶ前に capture_kind() で拡張子を検証しておくこと。
    """
    if index == 1:
        return base
    # `.wav.zst` は 1 つの拡張子として扱う（`out.wav-2.zst` にならないように）
    n = len(capture_kind(base)) + 1
    return base.with_name(f"{base.name[:-n]}-{index}{base.name[-n:]}")


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


# ---- デバイス検出 -------------------------------------------------------

def usb_interfaces():
    """IORegistry から USB インタフェースの一覧を取る。"""
    try:
        out = subprocess.run(
            ["ioreg", "-a", "-r", "-l", "-w", "0", "-c", "IOUSBHostInterface"],
            check=True, capture_output=True).stdout
    except OSError as e:
        raise RuntimeError(
            f"ioreg を実行できない: {e}"
            "（macOS 以外では --device / --pcm-device で明示すること）") from e
    except subprocess.CalledProcessError as e:
        raise RuntimeError(f"ioreg が失敗した: {e}") from e

    if not out.strip():
        return []
    try:
        entries = plistlib.loads(out)
    except Exception as e:
        raise RuntimeError(f"ioreg の出力を解釈できない: {e}") from e
    return entries if isinstance(entries, list) else [entries]


def callout_device(node):
    """インタフェース配下の IOSerialBSDClient が持つ /dev/cu.* を探す。"""
    dev = node.get("IOCalloutDevice")
    if dev:
        return dev
    for child in node.get("IORegistryEntryChildren", []):
        dev = callout_device(child)
        if dev:
            return dev
    return None


def find_cdc_devices(itf_name):
    """インタフェース名に対応する /dev/cu.* を全部返す。

    CDC-ACM は「制御インタフェース + データインタフェース」の 2 本組で、
    名前が付くのは制御側なのに tty がぶら下がるのはデータ側なので、名前の一致した
    インタフェースそのものではなく、同じデバイス（= 同じ locationID）で後ろに続く
    インタフェースから tty を拾う。
    """
    interfaces = usb_interfaces()
    found = []
    for itf in interfaces:
        if itf.get("IORegistryEntryName") != itf_name:
            continue
        loc = itf.get("locationID")
        num = itf.get("bInterfaceNumber")
        if loc is None or num is None:
            continue
        rest = [s for s in interfaces
                if s.get("locationID") == loc
                and s.get("bInterfaceNumber", -1) > num]
        for sib in sorted(rest, key=lambda s: s["bInterfaceNumber"]):
            dev = callout_device(sib)
            if dev:
                found.append(dev)
                break
    return found


def resolve_device(explicit, itf_name, wait_s=SERIAL_WAIT_S):
    """USB インタフェース名から /dev/cu.* を引く。explicit があればそれを優先する。

    SWD の reset 直後はデバイスが数秒消えるので、現れるまで待つ。
    """
    if explicit:
        return explicit

    deadline = time.monotonic() + wait_s
    while True:
        found = find_cdc_devices(itf_name)
        if len(found) == 1:
            return found[0]
        if len(found) > 1:
            raise RuntimeError(
                f'USB インタフェース "{itf_name}" が {len(found)} 個見つかった'
                f"（{', '.join(found)}）。"
                "--device / --pcm-device でどれを使うか明示すること")
        if time.monotonic() >= deadline:
            raise RuntimeError(
                f'USB インタフェース "{itf_name}" が見つからない。'
                "ファームが動いているか確認すること"
                "（--device / --pcm-device で明示も可）")
        time.sleep(0.2)


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

    def read_bytes(self, timeout):
        """届いているバイト列をそのまま返す。何も来なければ b""。

        PCM ストリームのように行の切れ目が無いデータ用。read_line() とは
        バッファを共有しないので、同じ fd で混ぜて使わないこと。
        """
        if timeout > 0 and not select.select([self.fd], [], [], timeout)[0]:
            return b""
        try:
            return os.read(self.fd, PCM_READ_BYTES)
        except BlockingIOError:
            return b""

    def drain_bytes(self, seconds):
        """指定時間ぶんバイト列を読み捨てる。"""
        deadline = time.monotonic() + seconds
        while True:
            remain = deadline - time.monotonic()
            if remain <= 0:
                return
            self.read_bytes(remain)

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
    """コマンドに応じた応答待ち時間。`d <ms>` と reset/c は余分に待つ。"""
    tokens = line.split()
    cmd = tokens[0].lower() if tokens else ""
    if cmd == "d" and len(tokens) >= 2 and tokens[1].isdigit():
        return REPLY_TIMEOUT_S + int(tokens[1]) / 1000.0
    if cmd in ("reset", "c"):
        return REPLY_TIMEOUT_S + 5.0
    if cmd == "p":
        # `p 0` はリングの残りを送り切ってから応答する（ファーム側の上限は 2 秒）
        return REPLY_TIMEOUT_S + PCM_DRAIN_TIMEOUT_S
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


def capture_command(step, args):
    """表示用のコマンドライン文字列。"""
    # 実行時は execute() が解決済みのデバイスパスを入れている。dry-run では
    # 実機を探しに行かないので、代わりに探す対象のインタフェース名を出す。
    return (f"p 1 → {args.pcm_device or CDC_PCM_NAME} ({step.ms} ms) → {step.output} "
            f"→ p 0")


def pcm_rate(args):
    """PCM のサンプリングレート [Hz]。YM3012 の出力レートは φM/64。"""
    return int(round(args.phim / 64.0))


def capture_byte_rate(args):
    """キャプチャ経路が 1 秒あたりに吐くバイト数。PCM は 1 フレーム 4 バイト（L と R の int16）。"""
    return float(pcm_rate(args) * PCM_FRAME_BYTES)


def expected_bytes(step, args):
    """要求どおりキャプチャできた場合に届くバイト数。φM が 0 なら None。"""
    rate = capture_byte_rate(args)
    return int(rate * step.ms / 1000.0) if rate else None


def report_output(step, args, captured):
    """キャプチャ長を検査して結果を表示する。エラーなら理由の文字列。

    captured は実際に受け取ったバイト数。ホストが PCM を読み落としても `p 0` の応答は
    `OK` で返るため、長さを見ないと短い出力を成功と誤認する。
    """
    if not step.output.exists():
        return f"キャプチャファイルが生成されていない: {step.output}"

    want = expected_bytes(step, args)
    rate = capture_byte_rate(args)
    if want is not None and captured < want * CAPTURE_SHORT_RATIO:
        return discard_output(
            step, f"キャプチャが短い: {captured / rate:.3f}s / "
                  f"要求 {step.ms / 1000.0:.3f}s ({100.0 * captured / want:.1f}%)")

    tail = f" / キャプチャ {captured / rate:.3f}s" if rate else ""
    print(f"* {step.output} ({step.output.stat().st_size} bytes){tail}", flush=True)
    return None


def run_capture(step, args, ser):
    """ファームの CDC #1 から PCM を読んで WAV に落とす。

    YM3012 の取り込みからデコードまでをファーム側で済ませる経路。
    取り込み中は PCM ポートを止めずに読み続ける必要がある。読まないと 65.5ms 分の
    DMA リングが溢れて overrun になる。
    """
    print(f"$ {capture_command(step, args)}", flush=True)
    step.output.parent.mkdir(parents=True, exist_ok=True)

    try:
        pcm = Serial(args.pcm_device, SERIAL_WAIT_S)
    except RuntimeError as e:
        return str(e)

    fp = closers = sink = None
    captured = 0
    try:
        # 前回の取りこぼしが残っていることがあるので、開始前に捨てる
        pcm.drain_bytes(0.2)

        err = send_line(ser, "p 1")
        if err is not None:
            return f"p 1 が失敗した: {err}"

        try:
            fp, closers = open_output(step.output, args.zstd_level)
        except OSError as e:
            send_line(ser, "p 0")
            return str(e)
        sink = WavSink(fp, 2, 2, pcm_rate(args))

        deadline = time.monotonic() + step.ms / 1000.0
        while True:
            remain = deadline - time.monotonic()
            if remain <= 0:
                break
            chunk = pcm.read_bytes(min(remain, 0.05))
            if chunk:
                sink.writeframes(chunk)
                captured += len(chunk)

        # p 0 の応答はドレインの完了を待つので、待つ間も PCM を読み続ける
        print("> p 0", flush=True)
        ser.write_line("p 0")
        reply = None
        drain_end = time.monotonic() + PCM_DRAIN_TIMEOUT_S
        while time.monotonic() < drain_end:
            chunk = pcm.read_bytes(0.01)
            if chunk:
                sink.writeframes(chunk)
                captured += len(chunk)
            line = ser.read_line(time.monotonic() + 0.005)
            if line is None:
                continue
            print(f"< {line}", flush=True)
            if line.startswith("#"):
                continue
            reply = line
            break

        # 応答の後もホスト側のバッファに残っていることがあるので拾い切る
        quiet_end = time.monotonic() + PCM_TAIL_QUIET_S
        while time.monotonic() < quiet_end:
            chunk = pcm.read_bytes(0.05)
            if chunk:
                sink.writeframes(chunk)
                captured += len(chunk)
                quiet_end = time.monotonic() + PCM_TAIL_QUIET_S

        if reply is None:
            return "p 0 の応答がタイムアウトした"
        if reply != "OK":
            return reply
    finally:
        if sink is not None:
            sink.close()
        for c in reversed(closers or []):
            c.close()
        pcm.close()

    return report_output(step, args, captured)


def discard_output(step, err):
    """失敗した回の出力を消す。長さの足りない WAV がデータセットに混ざる方が厄介。"""
    if step.output.exists():
        step.output.unlink()
        print(f"* 失敗したので {step.output} は消した", flush=True)
    return err


def execute(steps, args):
    ser = None
    n_sent = 0
    n_capture = 0
    n_error = 0

    try:
        if not args.dry_run:
            # デバイスパスは USB インタフェース名から引く。PCM 側は !capture が
            # あるときだけ要るので、無いシーケンスで見つからなくても失敗させない。
            try:
                args.device = resolve_device(args.device, CDC_COMMAND_NAME)
                if any(step.kind == "capture" for step in steps):
                    args.pcm_device = resolve_device(args.pcm_device, CDC_PCM_NAME)
            except RuntimeError as e:
                print(f"! {e}", file=sys.stderr, flush=True)
                return 1

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
                err = run_capture(step, args, ser)

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
                        help="キャプチャの出力ファイル名。拡張子で形式が決まる（.wav / .wav.zst）")
    parser.add_argument("-D", "--define", type=parse_define, action="append",
                        default=[], metavar="KEY=VALUE",
                        help="@KEY@ の置換値（複数指定可）")
    parser.add_argument("--device", default=None,
                        help=f'コマンド用 USB CDC のデバイス'
                             f'（既定は "{CDC_COMMAND_NAME}" から自動検出）')
    parser.add_argument("--pcm-device", default=None,
                        help=f'PCM 出力の USB CDC のデバイス'
                             f'（既定は "{CDC_PCM_NAME}" から自動検出）')
    parser.add_argument("--phim", type=float, default=DEFAULT_PHIM,
                        help=f"OPM の φM [Hz]（既定 {DEFAULT_PHIM:g}）")
    parser.add_argument("--zstd-level", type=int, default=DEFAULT_ZSTD_LEVEL,
                        metavar="N",
                        help=f".wav.zst の圧縮レベル（既定 {DEFAULT_ZSTD_LEVEL} = 最大）")
    parser.add_argument("-n", "--dry-run", action="store_true",
                        help="シリアルに触らず、実行内容だけ表示する")
    parser.add_argument("--stop-on-error", action="store_true",
                        help="エラーが出た時点で中断する（既定は警告して続行）")
    args = parser.parse_args(argv)

    defines = dict(args.define)

    # 拡張子は capture_path() が使うので、シーケンスを読む前に検証する
    if capture_kind(args.capture) is None:
        print(f"! キャプチャの出力名が .wav でも .wav.zst でもない: {args.capture}",
              file=sys.stderr)
        return 1

    try:
        steps, errors = preprocess(args.input, defines, args.capture)
    except OSError as e:
        print(f"! {e}", file=sys.stderr)
        return 1

    if errors:
        for e in errors:
            print(f"! {e}", file=sys.stderr)
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
