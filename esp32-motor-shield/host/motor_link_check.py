#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
motor_link_check.py —— 上位机: 检测「开发板 <-> 电机驱动板」是否连接成功

它做的事
--------
1. 枚举本机串口, 按 USB 芯片型号认出哪个是 ESP32/Arduino 板
2. 打开串口(默认 115200), 复位板子以抓开机自检报告
3. 自动判断板上烧的是哪个固件:
      MotorLinkCheck  -> 读它打印的 #JSON 报告, 直接给出 PASS/FAIL
      MotorDriver     -> 读它打印的 #STATUS, 看 shield 字段
      别的固件        -> 明确告诉你"串口有输出但不认识"
4. 需要时顺手执行点动测试 / 手动驱动电机

用法
----
    python motor_link_check.py                 # 检测链路(最常用)
    python motor_link_check.py --list          # 只列串口
    python motor_link_check.py --port COM9     # 指定串口
    python motor_link_check.py --test-motors    # 逐路点动 M1~M4
    python motor_link_check.py --drive 1 200   # 让 M1 以 200 正转
    python motor_link_check.py --stop          # 全部停止
    python motor_link_check.py --raw STATUS    # 发任意命令
    python motor_link_check.py --json          # 只输出一行 JSON, 方便脚本调用

退出码: 0 = 链路正常, 1 = 链路异常, 2 = 找不到可用串口
"""

import argparse
import json
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("缺少 pyserial。请先运行:  python -m pip install pyserial")
    sys.exit(2)

# Windows 控制台默认不是 UTF-8, 这里强制一下, 免得中文/emoji 报错
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

# ---------------------------------------------------------------------------
# 已知的 USB 转串口芯片 (ESP32/Arduino 板子上最常见的那几种)
# ---------------------------------------------------------------------------
KNOWN_BRIDGES = {
    (0x1A86, 0x7523): "CH340   (ESP32 开发板 / UNO 兼容板最常见)",
    (0x1A86, 0x5523): "CH341",
    (0x1A86, 0x55D4): "CH9102  (较新的 ESP32 板常用)",
    (0x1A86, 0x55D3): "CH343",
    (0x10C4, 0xEA60): "CP2102  (ESP32 DevKitC / NodeMCU 常见)",
    (0x10C4, 0xEA70): "CP2105",
    (0x0403, 0x6001): "FTDI FT232",
    (0x0403, 0x6015): "FTDI FT231X",
}
# 乐鑫原生 USB (ESP32-S2/S3/C3 内置 USB, 不需要转串口芯片)
ESPRESSIF_VID = 0x303A

# 明显不是我们要的串口
BT_HINTS = ("蓝牙", "Bluetooth", "BTHENUM")


def is_bluetooth(port):
    text = f"{port.description} {port.hwid}"
    return any(h.lower() in text.lower() for h in BT_HINTS)


def describe_port(port):
    """返回 (评分, 说明). 评分越高越像 ESP32/Arduino 板子."""
    vid = port.vid
    pid = port.pid
    desc = (port.description or "").strip()
    score = 0
    why = []

    if vid is not None and pid is not None:
        name = KNOWN_BRIDGES.get((vid, pid))
        if name:
            score += 50
            why.append(name)
        if vid == ESPRESSIF_VID:
            score += 60
            why.append("乐鑫原生 USB (ESP32-S2/S3/C3)")
        why.append(f"VID:PID = {vid:04X}:{pid:04X}")

    for kw, bonus in (("USB-SERIAL", 20), ("CH340", 20), ("CP210", 20),
                      ("Silicon Labs", 15), ("Espressif", 25), ("USB Serial", 15)):
        if kw.lower() in desc.lower():
            score += bonus
            why.append(f"描述含 '{kw}'")
            break

    if is_bluetooth(port):
        score -= 100
        why.append("是蓝牙虚拟串口, 不是开发板")

    if not why:
        why.append("未识别")
    return score, "; ".join(why)


def pick_port(explicit=None, allow_all=False):
    """挑一个最像开发板的串口, 返回 (device, 说明) 或 (None, 原因)"""
    ports = list(list_ports.comports())
    if not ports:
        return None, "本机没有枚举到任何串口"

    scored = [(describe_port(p), p) for p in ports]
    scored.sort(key=lambda t: t[0][0], reverse=True)

    if explicit:
        for (_, _), p in scored:
            if p.device.lower() == explicit.lower():
                return p.device, describe_port(p)[1]
        # 用户指定的串口不在列表里也照样试
        return explicit, "用户指定"

    best_score, best_port = scored[0][0][0], scored[0][1]
    if best_score <= 0 and not allow_all:
        return None, "PORTS_NOT_BOARD"

    return best_port.device, scored[0][0][1]


def list_ports_verbose():
    ports = list(list_ports.comports())
    if not ports:
        print("没有枚举到任何串口。")
        print("  -> 板子插上了吗? 换根 USB 线试试(有些线只供电不传数据)。")
        print("  -> 驱动装了吗? CH340 装 drivers\\CH341SER.EXE, CP210x 装 drivers\\CP210x\\")
        return

    print(f"本机共有 {len(ports)} 个串口:\n")
    rows = []
    for p in ports:
        (score, why) = describe_port(p)
        tag = "  <== 最像开发板" if score >= 50 else ("  (蓝牙, 忽略)" if score < 0 else "")
        rows.append((score, p.device, p.description or "", why, tag))
    rows.sort(key=lambda r: r[0], reverse=True)

    for score, dev, desc, why, tag in rows:
        print(f"  {dev:<8} {desc}")
        print(f"           {why}{tag}")
    print()


def read_for(ser, seconds, quiet=False):
    """在 seconds 秒内持续读取, 返回收到的文本"""
    buf = bytearray()
    deadline = time.time() + seconds
    while time.time() < deadline:
        n = ser.in_waiting
        if n:
            buf += ser.read(n)
            deadline = time.time() + 0.35   # 有数据就再等一会儿, 把一轮输出收完
        else:
            time.sleep(0.02)
    text = buf.decode("utf-8", errors="replace")
    if text and not quiet:
        for line in text.splitlines():
            if line.strip():
                print(f"    | {line}")
    return text


def hard_reset(ser):
    """模拟 esptool 的复位时序, 让板子重新跑一遍 setup(), 以便抓到开机自检"""
    try:
        ser.setDTR(False)
        ser.setRTS(True)
        time.sleep(0.12)
        ser.setRTS(False)
        time.sleep(0.06)
    except Exception:
        pass


def find_json_report(text):
    for line in reversed(text.splitlines()):
        line = line.strip()
        if line.startswith("#JSON "):
            try:
                return json.loads(line[len("#JSON "):])
            except json.JSONDecodeError:
                continue
    return None


def find_status_json(text):
    for line in reversed(text.splitlines()):
        line = line.strip()
        if line.startswith("#STATUS "):
            try:
                return json.loads(line[len("#STATUS "):])
            except json.JSONDecodeError:
                continue
    return None


def report_motorlink(data):
    """MotorLinkCheck 固件的报告"""
    print()
    print("=" * 62)
    print("  链路检测结果   (固件: MotorLinkCheck)")
    print("=" * 62)
    print(f"  开发板型号      : {data.get('board', '?')}")
    sda, scl = data.get("sda"), data.get("scl")
    if sda == -1 and scl == -1:
        print("  I2C 引脚        : 开发板默认引脚")
    else:
        print(f"  I2C 引脚        : SDA={sda}  SCL={scl}")
    print(f"  总线上器件数    : {data.get('scanCount')}"
          f"   (NACK {data.get('nack')} / 超时 {data.get('timeout')})")

    if data.get("shield"):
        print(f"  电机驱动板      : 在线, I2C 地址 0x{data.get('addr', 0):02X}")
        print()
        print("  结论: 开发板 <-> 电机驱动板  连接成功 ✔")
        print()
        print("  注意: 这只能证明【通信】通了。电机要真的转, 还需要:")
        print("        - 驱动板接上 6~12V 动力电源 (VM)")
        print("        - 电机接到 M1~M4 接线柱")
        print("        用 --test-motors 逐路点动来确认电机本体。")
    else:
        print("  电机驱动板      : 未检测到 ✘")
        print()
        print("  结论: 未能确认驱动板 (FAIL)")
        print("  排查:")
        print("    1) 驱动板是否完全插到底, 有没有插歪/虚接")
        print("    2) 驱动板上电源指示灯是否亮")
        print("    3) 板子的 I2C 引脚是否被改过 (可用 --raw 'I2C 21 22' 换引脚)")
        print("    4) 驱动板 I2C 地址是否被改过 (V5.6 支持改地址)")
    print("=" * 62)
    return 0 if data.get("shield") else 1


def report_motordriver(data, text):
    """MotorDriver 固件的报告"""
    shield = data.get("shield")
    print()
    print("=" * 62)
    print("  链路检测结果   (固件: MotorDriver)")
    print("=" * 62)
    print(f"  电机驱动板      : {'在线 ✔' if shield else '未检测到 ✘'}")
    if data.get("addr") is not None:
        print(f"  I2C 地址        : 0x{int(data['addr']):02X}")
    print(f"  电机状态        : M1={data.get('m1')} M2={data.get('m2')} "
          f"M3={data.get('m3')} M4={data.get('m4')}")
    print(f"  看门狗          : {data.get('wd')} ms (0=关闭)")
    print()
    if shield:
        print("  结论: 开发板 <-> 电机驱动板  连接成功 ✔")
    else:
        print("  结论: 驱动板未连接 (FAIL) —— 建议先烧 MotorLinkCheck 做完整排查")
    print("=" * 62)
    return 0 if shield else 1


def main():
    ap = argparse.ArgumentParser(
        description="检测开发板与电机驱动板之间的连接, 并可驱动电机",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="指定串口, 如 COM9")
    ap.add_argument("--baud", type=int, default=115200, help="波特率 (默认 115200)")
    ap.add_argument("--timeout", type=float, default=6.0, help="等待板子回应秒数")
    ap.add_argument("--list", action="store_true", help="只列出串口后退出")
    ap.add_argument("--no-reset", action="store_true", help="不复位板子(不抓开机自检)")
    ap.add_argument("--json", action="store_true", help="只输出一行 JSON 结果")
    ap.add_argument("--test-motors", action="store_true", help="逐路点动 M1~M4")
    ap.add_argument("--drive", nargs=2, type=int, metavar=("通道", "速度"),
                    help="驱动某路电机, 例: --drive 1 200")
    ap.add_argument("--stop", action="store_true", help="全部停止")
    ap.add_argument("--servo", nargs=2, type=int, metavar=("编号", "角度"),
                    help="设置舵机角度, 例: --servo 0 90")
    ap.add_argument("--raw", metavar="命令", help="发送任意一行命令")
    args = ap.parse_args()

    if args.list:
        list_ports_verbose()
        return 0

    device, why = pick_port(args.port)
    if device is None:
        if not args.json:
            print("!! 没有找到看起来像开发板的串口。\n")
            list_ports_verbose()
            print("如果板子确实插着, 用 --port COMx 手动指定。")
        else:
            print(json.dumps({"ok": False, "error": "no_port"}))
        return 2

    if not args.json:
        print(f"使用串口 {device}   ({why})")

    # 打开串口
    try:
        ser = serial.Serial(device, args.baud, timeout=0.1)
    except serial.SerialException as e:
        msg = str(e)
        if not args.json:
            print(f"\n!! 打不开 {device}: {msg}")
            if "Access is denied" in msg or "拒绝访问" in msg:
                print("   串口被占用了 —— 最常见的原因是 Arduino IDE 的【串口监视器】还开着,")
                print("   或者别的程序在用它。关掉再试。")
            elif "could not open port" in msg.lower():
                print("   串口不存在或已被拔掉。")
        else:
            print(json.dumps({"ok": False, "error": "open_failed", "detail": msg}))
        return 2

    try:
        # 复位 -> 抓开机自检; 然后主动问一次
        if not args.no_reset:
            hard_reset(ser)
        banner = read_for(ser, 1.2 if not args.no_reset else 0.3, quiet=args.json)

        ser.reset_input_buffer()
        ser.write(b"CHECK\n")
        ser.flush()
        text = read_for(ser, args.timeout, quiet=args.json)

        combined = banner + text

        # --- 情况 A: MotorLinkCheck 固件 ---
        data = find_json_report(combined)
        if data:
            if args.json:
                print(json.dumps(data, ensure_ascii=False))
                rc = 0 if data.get("shield") else 1
            else:
                rc = report_motorlink(data)
        # --- 情况 B: MotorDriver 固件 (它不认识 CHECK, 会回 ERR) ---
        elif "ERR unknown command" in combined or "PONG MotorDriver" in combined:
            ser.reset_input_buffer()
            ser.write(b"STATUS\n")
            ser.flush()
            text2 = read_for(ser, 2.5, quiet=args.json)
            st = find_status_json(combined + text2)
            if st:
                if args.json:
                    print(json.dumps(st, ensure_ascii=False))
                rc = report_motordriver(st, combined + text2)
            else:
                if not args.json:
                    print("\n板上是 MotorDriver 固件, 但没读到状态。")
                rc = 1
        # --- 情况 C: 有输出但不认识 ---
        elif combined.strip():
            if not args.json:
                print()
                print("串口有输出, 但不是 MotorLinkCheck / MotorDriver 固件。")
                print("请先烧录本项目的固件:  tools\\build.ps1 -Sketch MotorLinkCheck -Upload")
            else:
                print(json.dumps({"ok": False, "error": "unknown_firmware"}, ensure_ascii=False))
            rc = 1
        # --- 情况 D: 完全没反应 ---
        else:
            if not args.json:
                print()
                print("串口没有任何回应。可能是:")
                print("  1) 板子上没烧本项目的固件")
                print(f"  2) 波特率不对 (当前 {args.baud}, 本项目固件固定用 115200)")
                print("  3) 板子没在运行 / 卡住了")
                print("  4) 串口选错了 (用 --list 看看)")
            else:
                print(json.dumps({"ok": False, "error": "no_response"}, ensure_ascii=False))
            rc = 1

        # --- 附加动作 ---
        def send(cmd, wait=1.0, label=None):
            if not args.json and label:
                print(f"\n>> {label}")
            ser.reset_input_buffer()
            ser.write((cmd + "\n").encode())
            ser.flush()
            read_for(ser, wait, quiet=args.json)

        if args.raw:
            send(args.raw, 2.0, f"发送 {args.raw}")
        if args.test_motors:
            send("TEST ALL", 12.0, "逐路点动 M1~M4, 请看电机是否转动")
        if args.drive:
            ch, spd = args.drive
            send(f"M {ch} {spd}", 1.0, f"驱动 M{ch} = {spd}")
        if args.servo:
            n, a = args.servo
            send(f"SERVO {n} {a}", 1.0, f"舵机 {n} -> {a} 度")
        if args.stop:
            send("STOP", 1.0, "全部停止")

        return rc
    finally:
        ser.close()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n已中断。")
        sys.exit(130)
