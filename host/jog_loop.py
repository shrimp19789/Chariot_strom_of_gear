#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
jog_loop.py —— 连续重复点动，让驱动输出近似持续，便于"边测边看"

为什么要这个:
    MecanumPS2 的单次 JOG 上限是 800ms，而手持万用表来不及在 800ms 内读数，
    也很难看清轮子。本脚本以约 60ms 的间隔不停重发同一条 JOG，
    使驱动输出近似持续（占比约 93%），于是可以:
      1) 用万用表在点动期间测 STBY / IN / VM / OUT 的电平
      2) 肉眼确认轮子到底转不转

    输出是**实时**的：板上的状态行一有变化就立刻打印，长跑不会闷着没反应。

安全:
    - 只重发固件本来就接受的 JOG 命令，不放大占空比、不改时长上限
    - 退出前（含 Ctrl+C）一定发送 STOP，让板上回到 LOCKED
    - 四轮必须架空、投掷动力保持断开
    - 电机不转时是堵转状态，电流可能是空载的好几倍；长跑请摸一下
      TB6612 芯片和电机外壳，烫手就立刻停

用法 —— 必须在**仓库根目录**下执行（否则 host\\ 会被拼两次）:

    cd "D:\\desktop\\Obsidian Vaults\\新建文件夹\\chariot"

    python host\\jog_loop.py --wheel RL --duty 128 --seconds 300   # 长跑，配万用表
    python host\\jog_loop.py --wheel FL --duty 128 --seconds 6     # 短测，先看轮子

    如果你已经在 host\\ 目录里，就直接:
    python jog_loop.py --wheel RL --duty 128 --seconds 300
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


def emit(text, state):
    """实时打印一行；重复的 STATUS 只在变化时打印"""
    if text.startswith("STATUS "):
        if text != state["last_status"]:
            state["last_status"] = text
            print(text, flush=True)
    else:
        print(text, flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM9")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--wheel", default="RL", choices=["FL", "RL", "FR", "RR"])
    ap.add_argument("--duty", type=int, default=128,
                    help="占空比 1..128 (固件 v1.1 的单轮上限)")
    ap.add_argument("--duration", type=int, default=800,
                    help="单次点动时长 ms, 固件允许 50..800")
    ap.add_argument("--seconds", type=float, default=25.0,
                    help="总共持续多少秒; 配万用表建议 300")
    ap.add_argument("--gap", type=int, default=60,
                    help="两次点动之间的间隔 ms, 越小越接近连续输出")
    args = ap.parse_args()

    if not (1 <= args.duty <= 128):
        print("!! duty 必须在 1..128（固件单轮上限）", file=sys.stderr)
        sys.exit(2)
    if not (50 <= args.duration <= 800):
        print("!! duration 必须在 50..800ms", file=sys.stderr)
        sys.exit(2)

    cmd = f"JOG {args.wheel} {args.duty} {args.duration}\n".encode()
    period = (args.duration + args.gap) / 1000.0
    on_pct = args.duration / (args.duration + args.gap) * 100.0

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.1)
    except serial.SerialException as e:
        print(f"!! 打不开 {args.port}: {e}", file=sys.stderr)
        print("   常见原因: 串口被别的程序占用（Arduino IDE 串口监视器 / 上一次脚本没退出）",
              file=sys.stderr)
        sys.exit(2)

    state = {"last_status": None}
    sent = 0
    start = time.time()
    try:
        print(f"# 端口 {args.port} 已开；连续点动 {args.wheel} duty={args.duty}/255 "
              f"每次 {args.duration}ms，输出占比约 {on_pct:.0f}%", flush=True)
        print(f"# 共 {args.seconds:.0f} 秒。现在开始测电压 / 看轮子；"
              f"Ctrl+C 或在串口发 STOP 可停。", flush=True)
        print(f"# 测量点（黑笔接公共地，红笔点 D1 器件端针脚）:", flush=True)
        print(f"#   VM ≈ 电源设定值 | STBY ≈ 3.2V | "
              f"{args.wheel} 的 IN1 ≈ 占空比*3.3V | 两个输出端之间 ≈ 占空比*VM", flush=True)
        print("# 电机不转时属堵转，电流偏大；摸 TB6612 芯片和电机，烫手就停。", flush=True)
        print("", flush=True)

        pending = bytearray()

        def pump():
            """把已到达的字节收进缓冲，只输出完整的行（避免跨读块把行截断）"""
            n = ser.in_waiting
            if not n:
                return
            pending.extend(ser.read(n))
            while b"\n" in pending:
                line, _, rest = pending.partition(b"\n")
                pending.clear()
                pending.extend(rest)
                text = line.decode("utf-8", errors="replace").strip()
                if text:
                    emit(text, state)

        # 先读一小段静止状态
        t0 = time.time()
        while time.time() - t0 < 0.6:
            pump()
            time.sleep(0.01)

        deadline = start + args.seconds
        next_send = time.time()
        last_progress = time.time()

        while time.time() < deadline:
            now = time.time()
            if now >= next_send:
                ser.write(cmd)
                ser.flush()
                next_send = now + period
                sent += 1

            n = ser.in_waiting
            if n:
                pump()
            else:
                time.sleep(0.005)

            if now - last_progress >= 10.0:
                last_progress = now
                print(f"# 运行中 {now-start:.0f}s / {args.seconds:.0f}s"
                      f"（已发 {sent} 次点动）", flush=True)

        print(f"# 到时，共发送 {sent} 次点动，准备停止", flush=True)
    except KeyboardInterrupt:
        print("\n# 收到 Ctrl+C，停止", flush=True)
    finally:
        try:
            ser.write(b"STOP\n")
            ser.flush()
            t0 = time.time()
            while time.time() - t0 < 1.0:
                pump()
                time.sleep(0.01)
        except Exception:
            pass
        ser.close()

    print(f"---- 结束：已发送 STOP，共 {sent} 次点动，板子应回到 LOCKED ----")
    return 0


if __name__ == "__main__":
    sys.exit(main())
