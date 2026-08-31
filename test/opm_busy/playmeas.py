#!/usr/bin/env python3
"""曲を再生し、統計をリセットしてから一定時間走らせて `s` を読む。"""
import os, select, sys, time

DEV = "/dev/cu.usbmodem11201"


def main(song, settle=6.0, window=20.0):
    end = time.time() + 30
    while not os.path.exists(DEV) and time.time() < end:
        time.sleep(0.3)
    time.sleep(0.5)
    fd = os.open(DEV, os.O_RDWR | os.O_NONBLOCK | os.O_NOCTTY)

    def drain(t):
        buf = b""
        end = time.time() + t
        while time.time() < end:
            if select.select([fd], [], [], 0.1)[0]:
                try:
                    buf += os.read(fd, 65536)
                except BlockingIOError:
                    pass
        return buf.decode("utf-8", "replace")

    def send(c):
        os.write(fd, (c + "\r\n").encode())

    try:
        drain(0.3)
        send("mdx stop"); drain(0.5)
        send("mdx play " + song)
        pre = drain(settle)
        send("s 0"); drain(0.5)
        drain(window)
        send("s")
        out = drain(3.0)
        send("mdx stop"); drain(0.5)
        return pre, out
    finally:
        os.close(fd)


if __name__ == "__main__":
    pre, out = main(sys.argv[1] if len(sys.argv) > 1 else "DRACULA/dra03.mdx")
    sys.stdout.write(out)
