#!/usr/bin/env python3
"""ターゲットの CDC #0 へコマンドを送り、OK / ERR の行が出るまで読む。"""
import os, select, sys, time

DEV = "/dev/cu.usbmodem112101"


def run(cmds, timeout=60.0, settle=0.4):
    end = time.time() + 30
    while not os.path.exists(DEV) and time.time() < end:
        time.sleep(0.3)
    if not os.path.exists(DEV):
        raise SystemExit("device not found: " + DEV)
    time.sleep(settle)
    fd = os.open(DEV, os.O_RDWR | os.O_NONBLOCK | os.O_NOCTTY)
    try:
        end = time.time() + 0.3
        while time.time() < end:
            if select.select([fd], [], [], 0.05)[0]:
                try:
                    os.read(fd, 4096)
                except BlockingIOError:
                    pass
        out = []
        for c in cmds:
            os.write(fd, (c + "\r\n").encode())
            buf = b""
            done = False
            end = time.time() + timeout
            while not done and time.time() < end:
                if select.select([fd], [], [], max(0.0, end - time.time()))[0]:
                    try:
                        buf += os.read(fd, 65536)
                    except BlockingIOError:
                        pass
                    for line in buf.decode("utf-8", "replace").splitlines():
                        if line.strip() == "OK" or line.strip().startswith("ERR"):
                            done = True
                            break
            out.append(buf.decode("utf-8", "replace"))
        return out
    finally:
        os.close(fd)


if __name__ == "__main__":
    for r in run(sys.argv[1:]):
        sys.stdout.write(r)
