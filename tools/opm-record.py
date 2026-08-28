#!/usr/bin/env python3
"""
pico-opm-writer に入っている曲を WAV に録る。

`vgm play` / `mdx play` で 1 曲鳴らし、その頭から終わりまでをキャプチャして
1 曲 1 ファイルで保存する。曲の長さを事前に知る必要はない — 録り始めも録り終わりも
ファーム側が決める（[README.md](../README.md) §3.10 の `p 2` と §3.22 の曲の終わり方）。

## 何をするか

曲ごとに小さなシーケンスファイルを作り、[opm-writer.py](opm-writer.py) を
サブプロセスで起動する。シリアルの扱いと WAV の書き出しはあちらに任せる。

    <kind> loop <n>
    <kind> fade <ms>
    !capture-song <kind> play <path>
    <kind> stop

## 曲の指定

位置引数で指定する。`VGM:` / `MDX:` の接頭辞で種別を決め、無ければ拡張子
（`.vgm` / `.vgz` は VGM、`.mdx` は MDX）から判定する。

    tools/opm-record.py --loop 2 -o out/ VGM:GRADIUS.VGM MDX:SORCER/OP.MDX
    tools/opm-record.py --loop 2 -o out/ GRADIUS.VGM SORCER/OP.MDX

`--all` を付けると実機に入っている曲を全部録る。`--list` は一覧を TSV で出すだけで
録音はしない（`kind` / `size` / `path` の 3 列）。

外部ライブラリは使わない（標準ライブラリのみ）。

SPDX-License-Identifier: MIT
"""

import argparse
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

# 呼び出す実行系。同じ tools/ に置いてある前提
SCRIPT_DIR = Path(__file__).resolve().parent
OPM_WRITER = SCRIPT_DIR / "opm-writer.py"

# 種別の名前。ファームのコマンド名とそのまま同じ
KIND_VGM = "vgm"
KIND_MDX = "mdx"
KINDS = (KIND_VGM, KIND_MDX)

# 拡張子から種別を引く（接頭辞が無いときの判定）
EXT_KIND = {".vgm": KIND_VGM, ".vgz": KIND_VGM, ".mdx": KIND_MDX}

# 出力形式。opm-writer.py が拡張子で判断するのでそれに合わせる
FORMATS = ("wav.zst", "wav")

# `vgm list` / `mdx list` の 1 行。opm-writer.py が `< ` を付けて中継する
FILE_LINE_RE = re.compile(r"^<\s*#\s*file\s*:\s*(\d+)\s+(.*)$")

# 一覧の TSV の列
TSV_COLUMNS = ("kind", "size", "path")

# 既定値
DEFAULT_LOOP = 2
DEFAULT_FADE_MS = 2000
DEFAULT_FORMAT = "wav.zst"
DEFAULT_PHIM = 4000000.0
DEFAULT_ZSTD_LEVEL = 22
DEFAULT_RETRY = 3
DEFAULT_RETRY_WAIT_S = 5.0

# 一覧を取るときのサブプロセスの上限 [秒]。走査はファイル数に比例する
LIST_TIMEOUT_S = 120.0

# 1 曲の録音に許す時間 [秒]。opm-writer.py 側の上限に余裕を足したもの
RECORD_SLACK_S = 60.0


# ---- 曲の指定 -----------------------------------------------------------

class Song:
    """録る曲 1 本。kind は "vgm" / "mdx"、path は各ディレクトリからの相対パス。"""

    def __init__(self, kind, path):
        self.kind = kind
        self.path = path

    def __eq__(self, other):
        return (self.kind, self.path) == (other.kind, other.path)

    def __repr__(self):
        return f"{self.kind.upper()}:{self.path}"


def parse_song(spec):
    """位置引数 1 個を Song にする。判定できなければ ValueError。"""
    text = spec.strip()
    if not text:
        raise ValueError("曲の指定が空")

    # `:` は FAT のファイル名に使えないので、含まれていれば必ず種別の接頭辞
    head, sep, rest = text.partition(":")
    if sep:
        if head.lower() not in KINDS:
            raise ValueError(
                f"種別の接頭辞が VGM: でも MDX: でもない: {spec}")
        if not rest.strip():
            raise ValueError(f"曲名が空: {spec}")
        return Song(head.lower(), rest.strip())

    kind = EXT_KIND.get(Path(text).suffix.lower())
    if kind is None:
        raise ValueError(
            f"種別が決まらない: {spec}"
            "（VGM: / MDX: を付けるか、.vgm / .vgz / .mdx の名前にすること）")
    return Song(kind, text)


def output_path(song, out_dir, fmt):
    """曲 1 本の出力先。サブフォルダは `_` に潰して 1 階層に並べる。"""
    stem = song.path.replace("/", "_").replace("\\", "_")
    for ext in (".vgm", ".vgz", ".mdx", ".VGM", ".VGZ", ".MDX"):
        if stem.endswith(ext):
            stem = stem[: -len(ext)]
            break
    return Path(out_dir) / f"{stem}.{fmt}"


# ---- シーケンスの生成 ---------------------------------------------------

def build_sequence(song, loop, fade_ms):
    """曲 1 本ぶんのシーケンス。opm-writer.py に食わせる。"""
    return (
        f"# {song} を 1 曲ぶん録る（opm-record.py が生成）\n"
        f"{song.kind} loop {loop}\n"
        f"{song.kind} fade {fade_ms}\n"
        f"!capture-song {song.kind} play {song.path}\n"
        f"{song.kind} stop\n"
    )


def build_list_sequence(kind):
    """一覧を取るだけのシーケンス。"""
    return f"# {kind} の一覧を取る（opm-record.py が生成）\n{kind} list\n"


# ---- サブプロセス -------------------------------------------------------

def writer_argv(args, seq_path, out_path):
    """opm-writer.py のコマンドライン。"""
    argv = [str(OPM_WRITER), str(seq_path), str(out_path),
            "--phim", f"{args.phim:g}",
            "--zstd-level", str(args.zstd_level),
            "--song-max-ms", str(args.song_max_ms),
            "--stop-on-error"]
    if args.device:
        argv += ["--device", args.device]
    if args.pcm_device:
        argv += ["--pcm-device", args.pcm_device]
    if args.dry_run:
        argv += ["-n"]
    return argv


def run(argv, timeout, echo=True):
    """サブプロセスを 1 回走らせる。戻り値は (成功したか, 出力)。"""
    try:
        cp = subprocess.run(argv, timeout=timeout, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except subprocess.TimeoutExpired:
        return False, f"タイムアウト ({timeout:.0f}s): {' '.join(argv)}"
    except OSError as e:
        return False, f"起動できない: {e}"

    if echo:
        for line in cp.stdout.splitlines():
            print(f"| {line}", flush=True)
    return cp.returncode == 0, cp.stdout


# ---- 一覧 ---------------------------------------------------------------

def parse_list_output(kind, text):
    """opm-writer.py の出力から `# file : <size> <name>` を拾う。"""
    songs = []
    for line in text.splitlines():
        m = FILE_LINE_RE.match(line)
        if m:
            songs.append((Song(kind, m.group(2).strip()), int(m.group(1))))
    return songs


def fetch_list(args, kind, tmp):
    """実機から 1 種別の一覧を取る。戻り値は [(Song, size), ...]。"""
    seq = tmp / f"list_{kind}.txt"
    seq.write_text(build_list_sequence(kind), encoding="utf-8")
    # キャプチャは行わないが opm-writer.py は出力名を必須で取る
    ok, out = run(writer_argv(args, seq, tmp / "unused.wav"), LIST_TIMEOUT_S,
                  echo=False)
    if not ok:
        raise RuntimeError(f"{kind} list が失敗した:\n{out}")
    return parse_list_output(kind, out)


def collect(args, tmp):
    """--all / --list の対象を集める。"""
    kinds = KINDS if args.source == "both" else (args.source,)
    found = []
    for kind in kinds:
        found += fetch_list(args, kind, tmp)
    return found


# ---- 録音 ---------------------------------------------------------------

def record_one(song, args, tmp):
    """曲 1 本を録る。成功なら None、失敗なら理由の文字列。"""
    out = output_path(song, args.out, args.format)
    seq = tmp / "record.txt"
    seq.write_text(build_sequence(song, args.loop, args.fade), encoding="utf-8")

    timeout = args.song_max_ms / 1000.0 + RECORD_SLACK_S
    ok, out_text = run(writer_argv(args, seq, out), timeout)
    if not ok:
        return "opm-writer.py が失敗した"
    if not args.dry_run and not out.exists():
        return f"出力が生成されていない: {out}"
    return None


def record_all(songs, args, tmp):
    """全曲を録る。戻り値は失敗した件数。"""
    n_err = 0
    for i, song in enumerate(songs, start=1):
        print(f"--- [{i}/{len(songs)}] {song}", flush=True)
        err = None
        for attempt in range(args.retry + 1):
            if attempt:
                print(f"! リトライ {attempt}/{args.retry}: {song}",
                      file=sys.stderr, flush=True)
                time.sleep(args.retry_wait)
            err = record_one(song, args, tmp)
            if err is None:
                break
        if err is not None:
            n_err += 1
            print(f"! {song}: {err}", file=sys.stderr, flush=True)
    return n_err


# ---- 自己検証 -----------------------------------------------------------

def self_test():
    """実機に触れずに、指定の解釈・生成・一覧の解析を検証する。"""
    results = []

    def check(name, ok, detail=""):
        results.append((name, None if ok else (detail or "期待と違う")))

    # 曲の指定
    for spec, want in (
        ("VGM:A.VGM", Song("vgm", "A.VGM")),
        ("mdx:SUB/B.MDX", Song("mdx", "SUB/B.MDX")),
        ("A.VGM", Song("vgm", "A.VGM")),
        ("A.vgz", Song("vgm", "A.vgz")),
        ("SUB/B.mdx", Song("mdx", "SUB/B.mdx")),
        ("VGM: NAME WITH SPACE.VGM", Song("vgm", "NAME WITH SPACE.VGM")),
    ):
        got = parse_song(spec)
        check(f"曲の指定 {spec}", got == want, f"{got} != {want}")

    for spec in ("", "A.TXT", "VGM:", "ZZZ:A.VGM"):
        try:
            parse_song(spec)
            check(f"不正な指定を弾く {spec!r}", False, "例外が出ない")
        except ValueError:
            check(f"不正な指定を弾く {spec!r}", True)

    # 出力名
    for song, fmt, want in (
        (Song("vgm", "A.VGM"), "wav.zst", "A.wav.zst"),
        (Song("mdx", "SUB/B.MDX"), "wav", "SUB_B.wav"),
        (Song("mdx", "A B.mdx"), "wav", "A B.wav"),
    ):
        got = output_path(song, "out", fmt)
        check(f"出力名 {song}", got == Path("out") / want, f"{got} != out/{want}")

    # シーケンス
    seq = build_sequence(Song("mdx", "SUB/B.MDX"), 2, 1500)
    lines = [l for l in seq.splitlines() if l and not l.startswith("#")]
    check("シーケンスの行数", len(lines) == 4, str(lines))
    check("loop の行", lines[0] == "mdx loop 2", lines[0])
    check("fade の行", lines[1] == "mdx fade 1500", lines[1])
    check("capture-song の行", lines[2] == "!capture-song mdx play SUB/B.MDX", lines[2])
    check("stop の行", lines[3] == "mdx stop", lines[3])

    seq = build_sequence(Song("vgm", "A B.VGM"), 0, 0)
    check("空白を含む曲名がそのまま乗る",
          "!capture-song vgm play A B.VGM" in seq, seq)

    # 一覧の解析
    sample = "\n".join([
        "< # pico-opm-writer 0.2.0",
        "> mdx list",
        "< # file    :       937 BOSCONIAN/BOS01.MDX",
        "< # file    :      2436 SUB/A B.MDX",
        "< # warn    : truncated at 256 entries",
        "< # files   : 2",
        "< OK",
        "--- 送信 1 行 / キャプチャ 0 件 / エラー 0 件",
    ])
    got = parse_list_output("mdx", sample)
    check("一覧の件数", len(got) == 2, str(got))
    check("一覧の 1 件目",
          got and got[0] == (Song("mdx", "BOSCONIAN/BOS01.MDX"), 937), str(got[:1]))
    check("一覧は空白を含む名前を切らない",
          len(got) > 1 and got[1][0].path == "SUB/A B.MDX", str(got[1:]))
    check("一覧は file 以外の行を拾わない",
          all("files" not in s.path and "warn" not in s.path for s, _ in got), str(got))

    ng = 0
    for name, err in results:
        if err is None:
            print(f"PASS {name}")
        else:
            ng += 1
            print(f"FAIL {name}: {err}")
    print(f"--- {len(results)} ケース / NG {ng}")
    return 1 if ng else 0


# ---- エントリポイント ---------------------------------------------------

def main(argv=None):
    parser = argparse.ArgumentParser(
        description="pico-opm-writer に入っている曲を 1 曲 1 ファイルで WAV に録る")
    parser.add_argument("songs", nargs="*", metavar="SONG",
                        help="録る曲。VGM:<path> / MDX:<path>、または拡張子で判別できる名前")
    parser.add_argument("-o", "--out", type=Path, default=Path("."),
                        help="出力先ディレクトリ（既定はカレント）")
    parser.add_argument("--all", action="store_true",
                        help="実機に入っている曲を全部録る")
    parser.add_argument("--list", action="store_true",
                        help="実機に入っている曲の一覧を TSV で出す（録音はしない）")
    parser.add_argument("--source", choices=("vgm", "mdx", "both"), default="both",
                        help="--all / --list の対象ディレクトリ（既定 both）")
    parser.add_argument("--loop", type=int, default=DEFAULT_LOOP, metavar="N",
                        help=f"何周でフェードアウトして終わるか（既定 {DEFAULT_LOOP}、0 で無限）")
    parser.add_argument("--fade", type=int, default=DEFAULT_FADE_MS, metavar="MS",
                        help=f"フェードアウトの長さ [ms]（既定 {DEFAULT_FADE_MS}）")
    parser.add_argument("--format", choices=FORMATS, default=DEFAULT_FORMAT,
                        help=f"出力形式（既定 {DEFAULT_FORMAT}）")
    parser.add_argument("--phim", type=float, default=DEFAULT_PHIM,
                        help=f"OPM の φM [Hz]（既定 {DEFAULT_PHIM:g}）")
    parser.add_argument("--device", default=None, help="コマンド用 USB CDC のデバイス")
    parser.add_argument("--pcm-device", default=None, help="PCM 出力の USB CDC のデバイス")
    parser.add_argument("--zstd-level", type=int, default=DEFAULT_ZSTD_LEVEL, metavar="N",
                        help=f".wav.zst の圧縮レベル（既定 {DEFAULT_ZSTD_LEVEL} = 最大）")
    parser.add_argument("--song-max-ms", type=int, default=900000, metavar="MS",
                        help="1 曲に費やす上限 [ms]（既定 900000）")
    parser.add_argument("--retry", type=int, default=DEFAULT_RETRY, metavar="N",
                        help=f"1 曲あたりのリトライ回数（既定 {DEFAULT_RETRY}、0 で無効）")
    parser.add_argument("--retry-wait", type=float, default=DEFAULT_RETRY_WAIT_S,
                        metavar="SEC",
                        help=f"リトライまでの待ち時間 [秒]（既定 {DEFAULT_RETRY_WAIT_S:g}）")
    parser.add_argument("-n", "--dry-run", action="store_true",
                        help="シリアルに触らず、実行内容だけ表示する")
    parser.add_argument("--self-test", action="store_true",
                        help="実機なしで自己検証する")
    args = parser.parse_args(argv)

    if args.self_test:
        return self_test()

    if not OPM_WRITER.exists():
        print(f"! {OPM_WRITER} が見つからない", file=sys.stderr)
        return 1
    if args.retry < 0 or args.retry_wait < 0:
        print("! --retry / --retry-wait に負の値は指定できない", file=sys.stderr)
        return 1
    if args.songs and (args.all or args.list):
        print("! 曲を並べる指定と --all / --list は同時に使えない", file=sys.stderr)
        return 1
    if not args.songs and not args.all and not args.list:
        print("! 録る曲を指定するか、--all / --list を使うこと", file=sys.stderr)
        return 1

    tmp = tempfile.TemporaryDirectory()
    try:
        base = Path(tmp.name)
        if args.all or args.list:
            try:
                found = collect(args, base)
            except RuntimeError as e:
                print(f"! {e}", file=sys.stderr)
                return 1
            if args.list:
                print("\t".join(TSV_COLUMNS))
                for song, size in found:
                    print(f"{song.kind}\t{size}\t{song.path}")
                return 0
            songs = [song for song, _ in found]
        else:
            try:
                songs = [parse_song(s) for s in args.songs]
            except ValueError as e:
                print(f"! {e}", file=sys.stderr)
                return 1

        if not songs:
            print("! 録る曲が 1 本も見つからない", file=sys.stderr)
            return 1

        if not args.dry_run:
            args.out.mkdir(parents=True, exist_ok=True)

        n_err = record_all(songs, args, base)
        print(f"--- 曲 {len(songs)} 本 / エラー {n_err} 件", flush=True)
        return 1 if n_err else 0
    finally:
        tmp.cleanup()


if __name__ == "__main__":
    sys.exit(main())
