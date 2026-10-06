#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
serial_watch.py —— 打开串口, 复位开发板, 把输出打印出来

给 flash.ps1 做烧录后验证用, 也可以单独用来盯着串口看。

用法:
    python serial_watch.py --port COM9 --seconds 8
    python serial_watch.py --port COM9 --seconds 8 --no-reset
    python serial_watch.py --port COM9 --seconds 10 --send "c"
"""

import argparse
import sys
import time

try:
    import serial
except ImportError:
    print("缺少 pyserial, 请运行: python -m pip install pyserial", file=sys.stderr)
    sys.exit(2)

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def hard_reset(ser):
    """ESP32 自动复位时序: RTS 拉低 EN 复位, DTR 控制 IO0"""
    try:
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.12)
        ser.setRTS(False)
        time.sleep(0.06)
    except Exception:
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--seconds", type=float, default=8.0)
    ap.add_argument("--no-reset", action="store_true")
    ap.add_argument("--send", action="append", default=[],
                    help="在开始读取后发送的命令(可重复), 例如 --send c")
    ap.add_argument("--send-after", type=float, default=2.0,
                    help="开机后多少秒发送 --send 的命令")
    args = ap.parse_args()

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.1)
    except serial.SerialException as e:
        print(f"!! 打不开 {args.port}: {e}", file=sys.stderr)
        print("   常见原因: Arduino IDE 的串口监视器还开着, 占用了串口。", file=sys.stderr)
        sys.exit(2)

    sent = False
    try:
        if not args.no_reset:
            hard_reset(ser)

        start = time.time()
        deadline = start + args.seconds
        buf = bytearray()

        while time.time() < deadline:
            if (not sent) and args.send and (time.time() - start) >= args.send_after:
                for cmd in args.send:
                    ser.write((cmd + "\n").encode())
                ser.flush()
                sent = True

            n = ser.in_waiting
            if n:
                buf += ser.read(n)
                deadline = max(deadline, time.time() + 0.4)  # 有数据就再多等等
            else:
                time.sleep(0.02)

        text = buf.decode("utf-8", errors="replace")
        sys.stdout.write(text)
        if text and not text.endswith("\n"):
            sys.stdout.write("\n")
        return 0
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
