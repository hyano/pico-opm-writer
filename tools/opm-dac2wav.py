#!/usr/bin/env python3
"""
sigrok-cli でキャプチャした YM2151 (OPM) の DAC 出力信号を 16bit ステレオ WAV に変換する。

入力は次のコマンドで採った生ロジックデータ（1 サンプル 1 バイト、bit0..3 に CH0..CH3）:

    sigrok-cli -d fx2lafw --config samplerate=8MHz -O binary --time=50s -o XXX.bin

    CH0 = φ1 / CH1 = SO / CH2 = SH1 / CH3 = SH2

## 入出力

入力・出力ともに `-` で標準入力・標準出力を指せる。出力名が `.zst` で終わる場合は zstd で
圧縮して書く（既定はレベル 22 = 最大圧縮）。生キャプチャは 8MB/s で膨れるので、
ディスクに落とさずパイプで受けるのが既定の使い方:

    sigrok-cli -d fx2lafw --config samplerate=8MHz -O binary --time=10s \
      | opm-dac2wav.py - out.wav.zst

**標準出力はデータ専用**で、統計や警告はすべて標準エラー出力へ出す。

## YM3012 (DAC-MS) のシリアルフォーマット

データシート LSI-2130123 (1992-04) の記述そのまま。同じ線を OPM 側は SO、YM3012 側は
SD と呼ぶ。以下は OPM 側の名前 (SO) で通す。

- 1 変換 = φ1 16 クロック。CH1 → CH2 の順に繰り返すので 1 サンプル = 32 クロック。
  φ1 = φM/2 なので出力レートは φM/64（φM = 4MHz なら 62500Hz）。
- 16 スロットの中身は先に出る順に `無効×3, D0..D9, S0, S1, S2`。
- 仮数は LSB first で、最後に出る D9 が MSB。10bit オフセットバイナリなので
  符号付き値は m - 512。指数も LSB first で E = S2*4 + S1*2 + S0。
- 線形値は pcm = (m - 512) << (E - 1)。E = 0 は禁止コード（3→7 デコーダ）で 0 として扱う。
- SO は φ1 の立ち上がりで取り込まれるので、読むべきサンプルは「立ち上がり直前の L 区間」。
- SH2 の立ち下がりが CH1 データ列の終端、SH1 の立ち下がりが CH2 データ列の終端
  （Fig.3。各 SAM の H 区間は相手チャネルのデータ送出中に来るので直感と逆になる）。

## L/R の対応

OPM のレジスタ $20 の RL は MSB 側から R, L（bit7 = R / bit6 = L）。実機で片側ずつ鳴らすと
bit6 のみで CH2 が全サンプル厳密に 0、bit7 のみで CH1 が全サンプル厳密に 0 になる
（test/dac_lr の fast_4op_left / fast_4op_right）。よって既定は CH1 = L / CH2 = R。
基板の配線が逆なら --swap を使う。

**L と R は同一にならない。** 実測では、オペレータのレジスタオフセットに応じて
R[i] = L[i]（+0x00 / +0x08）と R[i] = L[i+1]（+0x10 / +0x18）に分かれる。CH2 のラッチが
CH1 の半フレーム後にあるためと考えられる。両群が均等に鳴っていると R は L の 2 タップ
移動平均 (L[i] + L[i+1]) / 2 になる。デコードの誤りではないので、波形解析には片側だけを
使うこと。詳細は test/dac_lr/README.md。

## デコードの流れ

キャプチャは 8MHz × 50s で 400MB になりうるので、重い処理はすべて C 側へ落としてある。

1. 1 バイトを「φ1 の H/L + SO/SH1/SH2」を表す ASCII シンボルへ translate する。
2. 正規表現の置換で φ1 の 1 周期を 1 シンボルへ圧縮する。残すのは立ち上がり直前のサンプル。
   φ1 の各相は 250ns、LA の周期は 125ns なので各相に必ず 1〜2 サンプル入り、H/L の交互出現は保証される。
3. 圧縮列では φ1 と SH1/SH2 が完全同期なので、SH の立ち下がりは厳密に 32 シンボル周期になる。
   最初の SH2 立ち下がりで位相を決めたら、あとはストライド 32 のスライスで 13 ビットを一括抽出する。
4. 13bit ワード → int16 の 8192 エントリ LUT を引いて PCM 化する。

外部ライブラリは使わない（標準ライブラリのみ）。zstd の圧縮には Python 3.14 の
`compression.zstd` を使う。

SPDX-License-Identifier: MIT
"""

import argparse
import array
import re
import struct
import sys
from pathlib import Path

try:                                    # Python 3.14+ (PEP 784)
    from compression.zstd import CompressionParameter, ZstdFile
except ImportError:                     # 3.13 以前は zstd 出力を諦める
    ZstdFile = None

# ---- 定数 ---------------------------------------------------------------

# 1 バイトを表すシンボル。φ1=L は 0x40 台、φ1=H は 0x50 台。下位 3bit に SO/SH1/SH2。
SYM_LOW = 0x40   # '@'..'G'
SYM_HIGH = 0x50  # 'P'..'W'

BIT_SO = 0
BIT_SH1 = 1
BIT_SH2 = 2

# φ1 の 1 周期（L 区間 + H 区間）を、L 区間の最後の 1 シンボルへ潰す
RE_CYCLE = re.compile(rb'[@-G]*([@-G])[P-W]+')

# 圧縮列に対する検索。SH2 = bit2、SH1 = bit1。
RE_SH2_FALL = re.compile(rb'[D-G][@-C]')  # 立ち下がり（L 側の位置 = start + 1）
RE_SH2_IS_L = re.compile(rb'[@-C]')       # 「H のはず」の位置に L が居たら不整合
RE_SH2_IS_H = re.compile(rb'[D-G]')
RE_SH1_IS_L = re.compile(rb'[@ADE]')
RE_SH1_IS_H = re.compile(rb'[BCFG]')

# 1 フレーム = φ1 32 クロック。CH1 の D0 を起点にしたときの各要素の位置
FRAME_CLOCKS = 32
CH2_OFFSET = 16     # CH2 の D0
WORD_BITS = 13      # D0..D9, S0..S2
SH2_HIGH_AT = 12    # CH1 の S2。ここで SH2 は H
SH2_LOW_AT = 13     # 次のシンボルでは L（＝ここが立ち下がり）
SH1_HIGH_AT = 28    # CH2 の S2
SH1_LOW_AT = 29

# 1 回の read + feed でパイプを読まずに居る時間が、OS のパイプ容量（64KiB = 8MB/s で
# 8ms ぶん）より短く収まるように選ぶ。ここが長いと sigrok-cli の write が待たされ、
# その間 libusb の転送が回らずロジアナ側が取りこぼす。64KiB で 1 回 3ms。
CHUNK_BYTES = 64 << 10
BLOCK_FRAMES = 1 << 16

# zstd の既定レベル。22 が上限（CLI の --ultra は API では不要）
ZSTD_LEVEL = 22

# WavSink がヘッダのサイズ欄を確定させるために溜めておける PCM の上限 [byte]。
# 64MiB = 62500Hz / 16bit / stereo で約 17 分ぶん。
WAV_BUFFER_MAX = 64 << 20
# ビット位相の自動判定に使うフレーム数。採点は Python ループなのでここだけ重く、
# 判定が走る 1 回の feed() が長くなる（上の CHUNK_BYTES の注意と同じ理由で短く保つ）。
PROBE_FRAMES = 2000
SCAN_LIMIT = 4096       # サイクル境界を Python ループで探す上限

# 13bit ワード → int16
PCM_LUT = [0 if (w >> 10) == 0 else ((w & 0x3FF) - 512) << ((w >> 10) - 1)
           for w in range(1 << WORD_BITS)]
SQ_LUT = [v * v for v in PCM_LUT]

# シンボルの bit0（SO）を 0/1 のバイトへ
TBL_BIT0 = bytes(i & 1 for i in range(256))
# シンボルから指数だけを取り出す（ワード上位バイト >> 2）
TBL_EXP = bytes((i >> 2) & 7 for i in range(256))


class DecodeError(RuntimeError):
    pass


# ---- 入出力 -------------------------------------------------------------

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


def open_input(path):
    """入力を開いて (fp, closers) を返す。`-` は標準入力（閉じない）。"""
    if str(path) == "-":
        return sys.stdin.buffer, []
    fp = open(path, "rb")
    return fp, [fp]


def open_output(path, level):
    """出力を開いて (fp, closers, 表示名) を返す。`.zst` なら zstd で圧縮する。

    `closers` は呼び出し側が逆順に閉じる（標準出力のときは空＝閉じない）。
    """
    name = str(path)
    compress = name.endswith(".zst")
    if compress and ZstdFile is None:
        # ファイルを作る前に弾く（空の出力を残さない）
        raise OSError("zstd 圧縮の出力には Python 3.14 以降が必要"
                      "（`... - - | zstd -22 --ultra -o FILE` で受けること）")

    if name == "-":
        raw, closers, display = sys.stdout.buffer, [], "<stdout>"
    else:
        raw = open(path, "wb")
        closers, display = [raw], name

    fp = raw
    if compress:
        fp = ZstdFile(raw, "wb",
                      options={CompressionParameter.compression_level: level})
        closers.append(fp)
    return fp, closers, display


# ---- 前処理 -------------------------------------------------------------

def make_symbol_table(bits):
    """生バイト → シンボルの 256 バイト変換表を作る。"""
    tbl = bytearray(256)
    for b in range(256):
        s = SYM_HIGH if (b >> bits["phi1"]) & 1 else SYM_LOW
        s |= ((b >> bits["so"]) & 1) << BIT_SO
        s |= ((b >> bits["sh1"]) & 1) << BIT_SH1
        s |= ((b >> bits["sh2"]) & 1) << BIT_SH2
        tbl[b] = s
    return bytes(tbl)


def first_cycle_start(seg):
    """先頭側から最初の H→L の変わり目（サイクルの先頭）を探す。"""
    for i in range(1, min(len(seg), SCAN_LIMIT)):
        if seg[i] < SYM_HIGH <= seg[i - 1]:
            return i
    return None


def last_cycle_start(seg):
    """末尾側から最後の H→L の変わり目を探す。ここから先は次チャンクへ繰り越す。"""
    for i in range(len(seg) - 1, max(0, len(seg) - SCAN_LIMIT), -1):
        if seg[i] < SYM_HIGH <= seg[i - 1]:
            return i
    return None


# ---- ビット抽出 ---------------------------------------------------------

def extract_words(seg, off, n):
    """seg[off + k + 32*i] (k = 0..12) を 1 フレーム 1 ワードにまとめる。

    1 フレームを 16bit のレーンに見立てた大整数を作り、ビットごとの列を OR で積む。
    ループは 13 回だけで、実際の処理はすべて C 側（スライス・translate・大整数演算）で走る。
    """
    acc = 0
    span = FRAME_CLOCKS * n
    for k in range(WORD_BITS):
        col = seg[off + k: off + k + span: FRAME_CLOCKS]
        lane = bytearray(2 * n)
        lane[1::2] = col.translate(TBL_BIT0)
        acc |= int.from_bytes(lane, "big") << k

    words = array.array("H")
    words.frombytes(acc.to_bytes(2 * n, "big"))
    if sys.byteorder == "little":
        words.byteswap()
    return words


def score_alignment(seg, base, n):
    """ビット位相の候補を採点する。小さいほど良い。

    第 1 基準は禁止コード E=0 の出現数（正しい位相なら 0 になるはず）。
    無音などで差が付かないときのために、隣接サンプルの差分和（波形の暴れ具合）で並べ替える。
    """
    bad = 0
    rough = 0
    for off in (base, base + CH2_OFFSET):
        words = extract_words(seg, off, n)
        bad += sum(1 for w in words if w < 1024)
        pcm = [PCM_LUT[w] for w in words]
        rough += sum(abs(a - b) for a, b in zip(pcm, pcm[1:]))
    return bad, rough


# ---- デコーダ本体 -------------------------------------------------------

class Decoder:
    def __init__(self, wav, tbl, delta=None, swap=False, max_frames=None):
        self.wav = wav
        self.tbl = tbl
        self.delta = delta
        self.swap = swap
        self.max_frames = max_frames

        self.sym_tail = b""      # サイクル途中で切れたぶんの繰り越し
        self.pending = bytearray()  # 圧縮済みシンボル列（未消費ぶん）
        self.base = None         # pending 内の「次フレームの CH1 D0」位置
        self.search_from = 0     # 同期点の探索開始位置
        self.head_done = False
        self.flushing = False

        # 統計
        self.raw_samples = 0
        self.dropped_samples = 0
        self.clocks = 0
        self.frames = 0
        self.resyncs = 0
        self.forbidden = 0
        self.exp_hist = [0] * 8
        self.peak = [0, 0]
        self.sumsq = [0, 0]
        self.scores = []
        self.dummy_hist = [0] * 8

    # -- 入力 --------------------------------------------------------

    def feed(self, chunk):
        """生データを 1 チャンク食わせる。まだ変換を続けるなら True。"""
        self.raw_samples += len(chunk)
        seg = self.sym_tail + chunk.translate(self.tbl)
        self.sym_tail = b""

        if not self.head_done:
            # キャプチャ開始位置は不定なので、最初の完全なサイクルまで捨てる
            start = first_cycle_start(seg)
            if start is None:
                if len(seg) >= SCAN_LIMIT:
                    raise DecodeError(
                        "φ1 のエッジが見つからない（--map のビット割り当てを確認すること）")
                self.sym_tail = seg
                return True
            self.dropped_samples += start
            seg = seg[start:]
            self.head_done = True

        end = last_cycle_start(seg)
        if end is None:
            if len(seg) > SCAN_LIMIT:
                raise DecodeError("φ1 がトグルしていない（--map のビット割り当てを確認すること）")
            self.sym_tail = seg
            return True
        self.sym_tail = seg[end:]

        compact = RE_CYCLE.sub(rb'\1', seg[:end])
        self.clocks += len(compact)
        self.pending.extend(compact)
        return self._drain()

    def finish(self):
        self.flushing = True
        if self.sym_tail:
            # 末尾の中途半端なサイクルは捨てる（H 区間が完結していない）
            self.sym_tail = b""
        self._drain()

    # -- 同期 --------------------------------------------------------

    def _sync_point(self, start):
        """start 以降で最初の SH2 立ち下がり位置（L 側のシンボル）を返す。"""
        m = RE_SH2_FALL.search(self.pending, start)
        while m is not None:
            i = m.start() + 1
            if i - WORD_BITS - 2 >= 0:
                return i
            m = RE_SH2_FALL.search(self.pending, i)
        return None

    def _choose_delta(self):
        """先頭付近でビット位相を総当たりし、いちばん筋の通る候補を選ぶ。"""
        i = self._sync_point(self.search_from)
        if i is None:
            return False

        avail = (len(self.pending) - (i - WORD_BITS) - FRAME_CLOCKS) // FRAME_CLOCKS
        n = min(PROBE_FRAMES, avail)
        if n < 1:
            return False
        if n < PROBE_FRAMES and not self.flushing:
            return False  # もう少しデータを溜めてから判定する

        best = None
        for d in (0, -1, 1, -2, 2):
            base = i - WORD_BITS + d
            if base < 0 or base + FRAME_CLOCKS * n + FRAME_CLOCKS > len(self.pending):
                continue
            score = score_alignment(self.pending, base, n)
            self.scores.append((d, score))
            if best is None or score < best[0]:
                best = (score, d)
        if best is None:
            return False

        self.delta = best[1]
        return True

    # -- 出力 --------------------------------------------------------

    def _drain(self):
        while True:
            if self.delta is None:
                if not self._choose_delta():
                    return True

            if self.base is None:
                i = self._sync_point(self.search_from)
                if i is None:
                    return True
                base = i - WORD_BITS + self.delta
                if base < 0:
                    self.search_from = i + 1
                    continue
                self.base = base

            n = (len(self.pending) - self.base - FRAME_CLOCKS) // FRAME_CLOCKS
            if n < 1:
                self._trim()
                return True
            n = min(n, BLOCK_FRAMES)
            if self.max_frames is not None:
                n = min(n, self.max_frames - self.frames)
                if n < 1:
                    return False

            ok = self._validate(self.base, n)
            if ok > 0:
                self._emit(self.base, ok)
            if ok < n:
                # 位相が崩れた。壊れたフレームの同期点は飛ばして次から取り直す
                self.resyncs += 1
                self.search_from = self.base + FRAME_CLOCKS * ok + CH2_OFFSET
                self.base = None
            else:
                self.base += FRAME_CLOCKS * n
            self._trim()

            if self.max_frames is not None and self.frames >= self.max_frames:
                return False

    def _validate(self, base, n):
        """SH1/SH2 の位相が保たれているフレーム数を返す（先頭から数えて）。"""
        span = FRAME_CLOCKS * n
        checks = (
            (SH2_HIGH_AT, RE_SH2_IS_L),
            (SH2_LOW_AT, RE_SH2_IS_H),
            (SH1_HIGH_AT, RE_SH1_IS_L),
            (SH1_LOW_AT, RE_SH1_IS_H),
        )
        good = n
        for off, violation in checks:
            col = self.pending[base + off: base + off + span: FRAME_CLOCKS]
            m = violation.search(col)
            if m is not None:
                good = min(good, m.start())
        return good

    def _emit(self, base, n):
        pcm = []
        for ch, off in enumerate((base, base + CH2_OFFSET)):
            words = extract_words(self.pending, off, n)
            hi = words.tobytes()[1::2] if sys.byteorder == "little" \
                else words.tobytes()[0::2]
            exps = hi.translate(TBL_EXP)
            for e in range(8):
                self.exp_hist[e] += exps.count(e)
            self.forbidden += exps.count(0)

            samples = array.array("h", map(PCM_LUT.__getitem__, words))
            self.peak[ch] = max(self.peak[ch], max(samples), -min(samples))
            self.sumsq[ch] += sum(map(SQ_LUT.__getitem__, words))
            pcm.append(samples)

        # 無効スロット（各データ列の直前 3bit）の中身を統計として拾っておく
        dummy = self.pending[base - 3: base - 3 + FRAME_CLOCKS * n: FRAME_CLOCKS] \
            if base >= 3 else b""
        for c in dummy.translate(TBL_BIT0):
            self.dummy_hist[c] += 1

        left, right = (pcm[1], pcm[0]) if self.swap else (pcm[0], pcm[1])
        inter = array.array("h", bytes(4 * n))
        inter[0::2] = left
        inter[1::2] = right
        if sys.byteorder == "big":
            inter.byteswap()
        self.wav.writeframes(inter.tobytes())
        self.frames += n

    def _trim(self):
        if self.base is not None:
            keep = self.base
        else:
            keep = self.search_from
        keep = max(0, keep - 64)
        if keep > 0:
            del self.pending[:keep]
            if self.base is not None:
                self.base -= keep
            self.search_from = max(0, self.search_from - keep)


# ---- 実行 ---------------------------------------------------------------

def parse_map(spec):
    bits = {"phi1": 0, "so": 1, "sh1": 2, "sh2": 3}
    for item in spec.split(","):
        item = item.strip()
        if not item:
            continue
        if "=" not in item:
            raise argparse.ArgumentTypeError(f"NAME=BIT の形式でない: {item}")
        name, val = item.split("=", 1)
        name = name.strip().lower()
        if name not in bits:
            raise argparse.ArgumentTypeError(f"未知の信号名: {name}")
        if not val.strip().isdigit() or not 0 <= int(val) <= 7:
            raise argparse.ArgumentTypeError(f"ビット番号は 0..7: {item}")
        bits[name] = int(val)
    if len(set(bits.values())) != 4:
        raise argparse.ArgumentTypeError(f"ビット番号が重複している: {spec}")
    return bits


def report(dec, args, rate):
    # 標準出力は WAV のデータ専用なので、統計はすべて標準エラー出力へ出す
    def out(msg):
        print(msg, file=sys.stderr)

    la = args.la_rate
    dur = dec.raw_samples / la if la else 0.0
    phi1 = dec.clocks / dur if dur else 0.0
    out(f"* 入力 {dec.raw_samples} サンプル ({dur:.3f}s @ {la / 1e6:g}MHz)")
    out(f"* φ1 {dec.clocks} クロック = {phi1 / 1e6:.4f}MHz (期待値 {args.phim / 2e6:g}MHz)")
    out(f"* フレーム {dec.frames} 個 = {dec.frames / rate:.3f}s @ {rate}Hz")
    out(f"* 捨てた冒頭 {dec.dropped_samples} サンプル / ビット位相 delta={dec.delta}")
    if dec.scores:
        detail = " ".join(f"{d:+d}:{s[0]}/{s[1]}" for d, s in dec.scores)
        out(f"* 位相候補 (禁止コード数/差分和) {detail}")
    out(f"* 再同期 {dec.resyncs} 回 / 禁止コード E=0 {dec.forbidden} 個")
    out("* 指数ヒストグラム " +
        " ".join(f"E{e}={dec.exp_hist[e]}" for e in range(8)))
    out(f"* 無効スロットの 1 の数 {dec.dummy_hist[1]} / 0 の数 {dec.dummy_hist[0]}")
    for ch, name in enumerate(("CH1", "CH2")):
        rms = (dec.sumsq[ch] / dec.frames) ** 0.5 if dec.frames else 0.0
        out(f"* {name} peak {dec.peak[ch]} / rms {rms:.1f}")

    if dur and abs(phi1 - args.phim / 2) > args.phim / 200:
        print("! φ1 の実測値が期待値から 1% 以上ずれている。"
              "--map / --phim / --la-rate を確認すること", file=sys.stderr)
    if dec.resyncs > dec.frames / 62500 + 1:
        print("! 再同期が多い。8MHz は φ1 に対して 4 サンプル/周期しかないため、"
              "16MHz でのキャプチャを検討すること", file=sys.stderr)


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="sigrok-cli でキャプチャした YM2151 の DAC 出力を WAV へ変換する")
    parser.add_argument("input", type=Path,
                        help="キャプチャファイル (-O binary)。- で標準入力")
    parser.add_argument("output", type=Path,
                        help="出力する WAV ファイル。- で標準出力、"
                             ".zst で終わる名前なら zstd で圧縮する")
    parser.add_argument("--phim", type=float, default=4000000.0,
                        help="OPM の φM [Hz]（既定 4000000。NTSC は 3579545）")
    parser.add_argument("--la-rate", type=float, default=8000000.0,
                        help="ロジアナのサンプルレート [Hz]（既定 8000000）")
    parser.add_argument("--map", type=parse_map, default=parse_map(""),
                        metavar="SPEC",
                        help="ビット割り当て（既定 phi1=0,so=1,sh1=2,sh2=3）")
    parser.add_argument("--swap", action="store_true",
                        help="CH1 を R、CH2 を L にする（既定は CH1=L / CH2=R）")
    parser.add_argument("--max-seconds", type=float, default=None,
                        help="先頭から指定秒数ぶんだけ変換する")
    parser.add_argument("--delta", type=int, default=None, choices=range(-2, 3),
                        metavar="N", help="ビット位相を自動判定せず固定する（-2..2）")
    parser.add_argument("--zstd-level", type=int, default=ZSTD_LEVEL, metavar="N",
                        help=f"出力が .zst のときの圧縮レベル（既定 {ZSTD_LEVEL} = 最大）")
    parser.add_argument("-v", "--info", action="store_true", help="統計を表示する")
    args = parser.parse_args(argv)

    rate = round(args.phim / 64)
    max_frames = None
    if args.max_seconds is not None:
        max_frames = max(1, int(args.max_seconds * rate))

    tbl = make_symbol_table(args.map)

    try:
        fp, closers, display = open_output(args.output, args.zstd_level)
    except OSError as e:
        print(f"! {e}", file=sys.stderr)
        return 1

    dec = Decoder(WavSink(fp, 2, 2, rate), tbl, delta=args.delta, swap=args.swap,
                  max_frames=max_frames)
    err = None
    try:
        in_fp, in_closers = open_input(args.input)
        try:
            while True:
                chunk = in_fp.read(CHUNK_BYTES)
                if not chunk:
                    break
                if not dec.feed(chunk):
                    break
            dec.finish()
        finally:
            for c in reversed(in_closers):
                c.close()
        if dec.frames == 0:
            err = "フレームを 1 つもデコードできなかった"
        else:
            dec.wav.close()
    except (DecodeError, OSError) as e:
        err = str(e)
    finally:
        for c in reversed(closers):
            try:
                c.close()
            except OSError:
                pass

    if err is not None:
        print(f"! {err}", file=sys.stderr)
        # 中途半端な出力を残さない。空の .wav.zst が残ると呼び出し側が完了と誤認する
        if str(args.output) != "-" and args.output.exists():
            args.output.unlink()
        return 1

    print(f"* {display} : {dec.frames} フレーム / {rate}Hz / 16bit stereo "
          f"({dec.frames / rate:.3f}s)", file=sys.stderr)
    if args.info:
        report(dec, args, rate)
    return 0


if __name__ == "__main__":
    sys.exit(main())
