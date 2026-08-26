#!/usr/bin/env python3
"""CDC#1 から PCM を取り込む。曲の頭から取れるよう、再生開始前にストリームを開く。"""
import os, select, sys, time

CMD = "/dev/cu.usbmodem112101"
PCM = "/dev/cu.usbmodem112103"


def capture(song, out, seconds=3.0):
    fc = os.open(CMD, os.O_RDWR | os.O_NONBLOCK | os.O_NOCTTY)

    def send(c):
        os.write(fc, (c + "\r\n").encode())

    def drainc(t):
        end = time.time() + t
        while time.time() < end:
            if select.select([fc], [], [], 0.05)[0]:
                try:
                    os.read(fc, 65536)
                except BlockingIOError:
                    pass

    try:
        drainc(0.3)
        send("mdx stop"); drainc(0.5)
        fp = os.open(PCM, os.O_RDONLY | os.O_NONBLOCK | os.O_NOCTTY)
        try:
            send("p 1"); drainc(0.5)
            # 開いた直後の残りを捨てる
            end = time.time() + 0.5
            while time.time() < end:
                if select.select([fp], [], [], 0.05)[0]:
                    try:
                        os.read(fp, 65536)
                    except BlockingIOError:
                        pass
            send("mdx play " + song)
            buf = bytearray()
            end = time.time() + seconds
            while time.time() < end:
                if select.select([fp], [], [], 0.05)[0]:
                    try:
                        buf += os.read(fp, 1 << 16)
                    except BlockingIOError:
                        pass
        finally:
            os.close(fp)
        send("mdx stop"); drainc(0.3)
        send("p 0"); drainc(0.3)
        open(out, "wb").write(bytes(buf))
        return len(buf)
    finally:
        os.close(fc)


if __name__ == "__main__":
    n = capture(sys.argv[1], sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 3.0)
    print("captured %d bytes (%d frames)" % (n, n // 4))
