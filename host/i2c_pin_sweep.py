#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
i2c_pin_sweep.py —— 远程扫不同的 I2C 引脚组合, 找 AS5600 到底接在哪两个 GPIO 上

背景: 板上 Stage1 固件支持 "P <sda> <scl>" 命令切换引脚并重新诊断。
      引脚是厂家让用户"按程序指定引脚接"的, 所以 21/22 只是最可能的猜测。

先扫 (22,21) 是为了验证一个很常见的故障: 【SDA/SCL 接反了】。

用法（在仓库根目录下执行）:  python host\i2c_pin_sweep.py --port COM9
"""

import argparse
import re
import sys
import time

import serial

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

# 要尝试的引脚组合。第一个是"反接"假设, 其余是 ESP32 上常见的 I2C 备选。
PIN_PAIRS = [
    (22, 21),   # SDA/SCL 接反的假设
    (21, 22),   # ESP32 默认 (已知失败, 作为对照)
    (18, 19),   # 常见备选
    (32, 33),   # 常见备选
    (25, 26),   # 若编码器和电机引脚混用 (驱动已断电, 安全)
    (4, 15),    # 少数板子
]


def read_for(ser, seconds):
    buf = bytearray()
    deadline = time.time() + seconds
    while time.time() < deadline:
        n = ser.in_waiting
        if n:
            buf += ser.read(n)
            deadline = time.time() + 0.3
        else:
            time.sleep(0.02)
    return buf.decode("utf-8", errors="replace")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM9")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--per-pair", type=float, default=4.0)
    args = ap.parse_args()

    ser = serial.Serial(args.port, args.baud, timeout=0.1)
    try:
        time.sleep(0.5)
        ser.reset_input_buffer()

        results = []
        for sda, scl in PIN_PAIRS:
            cmd = f"P {sda} {scl}\n"
            ser.write(cmd.encode())
            ser.flush()
            out = read_for(ser, args.per_pair)

            ok = "AS5600 已在 0x36 应答" in out
            found = re.search(r"结果: 找到 (\d+) 个器件 \(NACK=(\d+) 超时=(\d+)\)", out)
            elec = re.search(r"空闲电平=(\d).*?空闲电平=(\d)", out)
            addrs = re.findall(r"\[器件\] (0x[0-9A-Fa-f]{2})", out)

            results.append({
                "pair": f"({sda},{scl})",
                "ok": ok,
                "found": int(found.group(1)) if found else None,
                "nack": int(found.group(2)) if found else None,
                "timeout": int(found.group(3)) if found else None,
                "idle": f"SDA={elec.group(1)} SCL={elec.group(2)}" if elec else "?",
                "addrs": addrs,
            })
            print(f"  SDA={sda:<3} SCL={scl:<3} -> "
                  f"{'*** 找到 AS5600 ***' if ok else '无'}"
                  f"  空闲={results[-1]['idle']}"
                  f"  器件数={results[-1]['found']}"
                  f"  NACK={results[-1]['nack']} 超时={results[-1]['timeout']}"
                  + (f"  地址={results[-1]['addrs']}" if addrs else ""))

        # 恢复默认引脚, 免得把板子留在奇怪状态
        ser.write(b"P 21 22\n")
        ser.flush()
        read_for(ser, 1.5)

        print()
        hit = [r for r in results if r["ok"]]
        if hit:
            print(f"结论: AS5600 在 {hit[0]['pair']} 上应答了 (SDA, SCL)")
            print("      -> 把固件里的 SDA_PIN/SCL_PIN 改成这两个值即可")
        else:
            print("结论: 试过的引脚组合都没有找到 AS5600。")
            print("      这基本排除了'引脚不对'和'SDA/SCL 接反', 更可能是:")
            print("        - 编码器没有供电 (VCC 取自降压模块, 而驱动功率被断开了)")
            print("        - 编码器 GND 没有和 ESP32 共地")
            print("        - 编码器模块本身有问题")
            print("      最值得先查: 编码器的 VCC 到底接在哪里? 模块上的电源灯亮不亮?")
        return 0 if hit else 1
    finally:
        ser.close()


if __name__ == "__main__":
    sys.exit(main())
