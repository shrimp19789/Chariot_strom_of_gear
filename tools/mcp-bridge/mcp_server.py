#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Codex 串口 / Arduino MCP 桥（tools/mcp-bridge）—— 让 Codex 直接操作串口与 Arduino 工具链

它做什么
--------
把「检测串口 / 认芯片 / 编译 / 烧录 / 读串口 / 发命令」这些能力
包成 MCP 工具, 注册给 Codex CLI 后, Codex 就能自己完成:

    看板子插在哪个口 -> 问芯片是什么 -> 编译 -> 烧录 -> 复位读串口 -> 发命令交互

设计要点
--------
* 只用 stdio 讲 JSON-RPC, 任何日志都走 stderr(stdout 必须干净, 否则协议会坏)
* 不依赖 PowerShell, 直接用 pyserial + subprocess 调 arduino-cli / esptool
* 串口选择是"宁可不选也不乱选": 认不出 USB 芯片就报错让人指定
* 所有路径都可用环境变量覆盖, 默认按本机现状

本机路径默认值(可用环境变量覆盖):
    ARDUINO_PORTABLE_HOME    默认 D:\\Download\\arduino
    ARDUINO_SKETCH_ROOT      默认本仓库根目录   (相对路径的解析基准)
"""

import json
import os
import re
import subprocess
import sys
import time

import serial
from serial.tools import list_ports

from mcp.server.mcpserver import MCPServer

try:
    from mcp_types import ToolAnnotations
except ImportError:                       # 兼容旧版布局
    from mcp.types import ToolAnnotations

# ---------------------------------------------------------------------------
# 工具注解 —— 决定 Codex 要不要弹审批
#
#   read_only_hint=True 的工具, Codex 可以自动执行, 不打断你;
#   不标的工具(烧录、发串口命令、逃生舱)会照常弹审批。
#
# 这是一个刻意的取舍: 侦查类操作(看串口、问芯片)零摩擦,
# 而任何"会改变硬件状态"的操作(尤其串口发命令可能让电机转起来)
# 都必须经过你同意。
# ---------------------------------------------------------------------------
READ_ONLY = ToolAnnotations(read_only_hint=True, idempotent_hint=True)

# ---------------------------------------------------------------------------
# 路径配置
# ---------------------------------------------------------------------------
ARDUINO_HOME = os.environ.get("ARDUINO_PORTABLE_HOME", r"D:\Download\arduino")
# 默认以仓库根作为相对路径的解析基准 (本文件在 tools/mcp-bridge/ 下, 上三级即仓库根)
_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SKETCH_ROOT = os.environ.get("ARDUINO_SKETCH_ROOT", _REPO_ROOT)

ARDUINO_CLI = os.path.join(
    ARDUINO_HOME, "resources", "app", "lib", "backend", "resources", "arduino-cli.exe")
ARDUINO_CFG = os.path.join(ARDUINO_HOME, "data", "arduino-cli.yaml")

DEFAULT_BAUD = 115200
DEFAULT_FQBN = "esp32:esp32:esp32"


def find_esptool():
    """在 esp32 工具链里找 esptool.exe (版本号会变, 所以动态找)"""
    base = os.path.join(ARDUINO_HOME, "data", "packages", "esp32", "tools", "esptool_py")
    if not os.path.isdir(base):
        return None
    for ver in sorted(os.listdir(base), reverse=True):
        exe = os.path.join(base, ver, "esptool.exe")
        if os.path.isfile(exe):
            return exe
    return None


ESPTOOL = find_esptool()

# 已知 USB 转串口芯片
KNOWN_BRIDGES = {
    (0x1A86, 0x7523): "CH340",
    (0x1A86, 0x5523): "CH341",
    (0x1A86, 0x55D4): "CH9102",
    (0x1A86, 0x55D3): "CH343",
    (0x10C4, 0xEA60): "CP2102",
    (0x10C4, 0xEA70): "CP2105",
    (0x0403, 0x6001): "FTDI FT232",
    (0x0403, 0x6015): "FTDI FT231X",
}
ESPRESSIF_VID = 0x303A
BT_HINTS = ("蓝牙", "Bluetooth", "BTHENUM")


# ---------------------------------------------------------------------------
# 小工具
# ---------------------------------------------------------------------------
def _log(msg):
    """日志只能走 stderr —— stdout 是 MCP 协议通道, 写脏了协议就崩"""
    print(msg, file=sys.stderr, flush=True)


def _run(cmd, timeout=300, cwd=None):
    """跑一个命令, 返回 (returncode, 合并后的输出)"""
    _log(f"[bridge] exec: {' '.join(str(c) for c in cmd)}")
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout, cwd=cwd)
    except subprocess.TimeoutExpired:
        return -1, f"(超时 {timeout}s): {' '.join(str(c) for c in cmd)}"
    except FileNotFoundError as e:
        return -2, f"(找不到程序): {e}"
    out = (p.stdout or b"") + (p.stderr or b"")
    return p.returncode, out.decode("utf-8", errors="replace")


def _resolve_sketch(sketch):
    """把 sketch 参数解析成绝对路径。可以给目录, 也可以给 .ino 文件"""
    p = sketch
    if not os.path.isabs(p):
        p = os.path.join(SKETCH_ROOT, p)
    p = os.path.normpath(p)
    if os.path.isfile(p) and p.lower().endswith(".ino"):
        p = os.path.dirname(p)
    return p


def _classify_port(port):
    """给串口打分并给出理由"""
    vid, pid = port.vid, port.pid
    desc = (port.description or "").strip()
    score, why = 0, []

    if vid is not None and pid is not None:
        name = KNOWN_BRIDGES.get((vid, pid))
        if name:
            score += 50
            why.append(name)
        if vid == ESPRESSIF_VID:
            score += 60
            why.append("乐鑫原生USB")
        why.append(f"VID:PID={vid:04X}:{pid:04X}")

    if any(h.lower() in f"{desc} {port.hwid}".lower() for h in BT_HINTS):
        score = -100
        why.append("蓝牙虚拟串口")
    elif "USB" in desc.upper() or "CH34" in desc.upper() or "CP210" in desc.upper():
        score += 20

    return score, "; ".join(why) if why else "未识别"


def _pick_port(explicit=None):
    """挑串口。返回 (device, 说明) 或 (None, 错误信息)"""
    ports = list(list_ports.comports())
    if not ports:
        return None, "本机没有枚举到任何串口"

    if explicit:
        return explicit, "用户指定"

    scored = sorted(((_classify_port(p)[0], p) for p in ports),
                    key=lambda t: t[0], reverse=True)
    best_score, best = scored[0]
    if best_score < 50:
        detail = "\n".join(f"  {p.device}: {_classify_port(p)[1]}" for _, p in scored)
        return None, ("没有认得出 USB 转串口芯片的串口, 不敢乱选。现有串口:\n" + detail +
                      "\n请用 port 参数显式指定, 例如 port='COM9'")
    return best.device, _classify_port(best)[1]


def _fqbn_for_chip(chip):
    if not chip:
        return DEFAULT_FQBN
    if "ESP32-S3" in chip: return "esp32:esp32:esp32s3"
    if "ESP32-S2" in chip: return "esp32:esp32:esp32s2"
    if "ESP32-C3" in chip: return "esp32:esp32:esp32c3"
    if "ESP32-C6" in chip: return "esp32:esp32:esp32c6"
    if "ESP32-P4" in chip: return "esp32:esp32:esp32p4"
    return DEFAULT_FQBN


def _chip_info_raw(port):
    """调 esptool 问芯片, 返回 (ok, 原始输出)"""
    if not ESPTOOL:
        return False, "找不到 esptool.exe (检查 ARDUINO_PORTABLE_HOME)"
    rc, out = _run([ESPTOOL, "--port", port, "flash-id"], timeout=90)
    return rc == 0, out


def _parse_chip(out):
    chip = re.search(r"Chip type:\s*(.+)", out)
    mbus = re.search(r"Detected flash size:\s*(\d+)MB", out)
    mac = re.search(r"(?m)^MAC:\s*(\S+)", out)
    return (chip.group(1).strip() if chip else None,
            int(mbus.group(1)) if mbus else None,
            mac.group(1) if mac else None)


# ---------------------------------------------------------------------------
# MCP 服务器
# ---------------------------------------------------------------------------
mcp = MCPServer(
    name="arduino-serial-bridge",
    version="1.0.0",
    instructions=(
        "直接操作本机的串口与 Arduino 工具链。典型流程: "
        "list_serial_ports -> chip_info -> arduino_upload -> serial_read。"
        "串口默认 115200; 板子上如果是本项目固件, 命令用文本行(以 \\n 结尾)。"
        "注意: 烧录会覆盖板上现有固件, 调用前先确认。"
    ),
)


@mcp.tool(annotations=READ_ONLY)
def list_serial_ports() -> str:
    """列出本机所有串口, 标出 USB 芯片型号, 并自动排除蓝牙虚拟串口。

    用于第一步: 找出开发板插在哪个 COM 口。
    """
    ports = list(list_ports.comports())
    if not ports:
        return "没有枚举到任何串口。检查数据线(有些只能充电)、驱动(CH341SER/CP210x)、板子是否插好。"

    rows = []
    for p in ports:
        score, why = _classify_port(p)
        tag = "  <== 最像开发板" if score >= 50 else ("  (蓝牙,忽略)" if score < 0 else "")
        rows.append((score, p.device, p.description or "", why, tag))
    rows.sort(key=lambda r: r[0], reverse=True)

    lines = [f"共 {len(ports)} 个串口:", ""]
    for score, dev, desc, why, tag in rows:
        lines.append(f"{dev:<8} {desc}")
        lines.append(f"         {why}{tag}")
    return "\n".join(lines)


@mcp.tool(annotations=READ_ONLY)
def chip_info(port: str = "") -> str:
    """用 esptool 读芯片型号/版本/Flash 大小/MAC, 并给出建议的 FQBN。

    这是选对编译目标的关键一步: 接线图写错板型时, 以这里读到的为准。
    port 留空则自动挑一个像开发板的串口。
    """
    dev, why = _pick_port(port or None)
    if dev is None:
        return why

    ok, out = _chip_info_raw(dev)
    if not ok:
        return f"无法与 {dev} 上的芯片通信。\n{out}\n\n检查: 串口是否被占用(如 Arduino IDE 监视器)? 驱动? 数据线?"

    chip, mbus, mac = _parse_chip(out)
    lines = [f"串口: {dev}  ({why})"]
    if chip:  lines.append(f"芯片: {chip}")
    if mbus:  lines.append(f"Flash: {mbus} MB")
    if mac:   lines.append(f"MAC: {mac}")
    lines.append(f"建议 FQBN: {_fqbn_for_chip(chip)}")
    return "\n".join(lines)


@mcp.tool()
def arduino_compile(sketch: str, fqbn: str = "") -> str:
    """编译一个 sketch(不烧录)。

    sketch: sketch 目录或 .ino 文件的路径; 相对路径按 SKETCH_ROOT 解析。
    fqbn:   留空则用 esp32:esp32:esp32。
    """
    path = _resolve_sketch(sketch)
    if not os.path.isdir(path):
        return f"找不到 sketch 目录: {path}"

    cmd = [ARDUINO_CLI, "compile", "--config-file", ARDUINO_CFG,
           "--fqbn", fqbn or DEFAULT_FQBN, "--warnings", "default", path]
    rc, out = _run(cmd, timeout=600)
    if rc == 0:
        size = [l for l in out.splitlines() if "Sketch uses" in l or "Global variables" in l]
        return f"编译成功 ({path})\n" + "\n".join(size)
    errs = [l for l in out.splitlines() if "error" in l.lower()][:15]
    return f"编译失败 (rc={rc})\n" + "\n".join(errs or out.splitlines()[-15:])


@mcp.tool()
def arduino_upload(sketch: str, port: str = "", fqbn: str = "", upload_speed: int = 0) -> str:
    """编译并烧录到板子。会覆盖板上现有固件, 调用前请确认。

    sketch:       sketch 目录或 .ino 文件路径
    port:         串口, 留空自动挑
    fqbn:         留空则先问芯片再决定(推荐留空)
    upload_speed: 上传波特率, 0=用默认; 下载老是失败可试 115200
    """
    path = _resolve_sketch(sketch)
    if not os.path.isdir(path):
        return f"找不到 sketch 目录: {path}"

    dev, why = _pick_port(port or None)
    if dev is None:
        return why

    use_fqbn = fqbn
    if not use_fqbn:
        ok, out = _chip_info_raw(dev)
        chip = _parse_chip(out)[0] if ok else None
        use_fqbn = _fqbn_for_chip(chip)
    if upload_speed:
        use_fqbn = f"{use_fqbn}:UploadSpeed={upload_speed}"

    cmd = [ARDUINO_CLI, "compile", "--config-file", ARDUINO_CFG,
           "--fqbn", use_fqbn, "--upload", "--port", dev, path]
    rc, out = _run(cmd, timeout=900)

    tail = out.splitlines()[-12:]
    if rc == 0:
        return f"烧录成功\n串口: {dev} ({why})\nFQBN: {use_fqbn}\n\n" + "\n".join(tail)
    return (f"烧录失败 (rc={rc})\n串口: {dev}\nFQBN: {use_fqbn}\n"
            + "\n".join(out.splitlines()[-25:])
            + "\n\n若失败在下载阶段而非编译阶段, 重试时带 upload_speed=115200")


@mcp.tool()
def serial_read(port: str = "", seconds: float = 8.0, reset: bool = True,
                send: str = "", baud: int = DEFAULT_BAUD) -> str:
    """打开串口读一段时间的输出, 可选先复位板子以抓开机日志。

    reset=True  会模拟 esptool 的复位时序, 让板子重跑 setup() —— 抓开机自检用
    send        要发送的命令; 多条用 | 分隔, 例如 "CHECK|STATUS"
    """
    dev, why = _pick_port(port or None)
    if dev is None:
        return why

    try:
        ser = serial.Serial(dev, baud, timeout=0.1)
    except serial.SerialException as e:
        return (f"打不开 {dev}: {e}\n"
                "最常见原因: Arduino IDE 的串口监视器还开着占用了串口。")

    try:
        if reset:
            try:
                ser.setDTR(False); ser.setRTS(True); time.sleep(0.12)
                ser.setRTS(False); time.sleep(0.06)
            except Exception:
                pass

        cmds = [c for c in (send or "").split("|") if c.strip()]
        buf = bytearray()
        start = time.time()
        deadline = start + seconds
        sent = False

        while time.time() < deadline:
            if cmds and not sent and (time.time() - start) >= 1.0:
                for c in cmds:
                    ser.write((c.strip() + "\n").encode())
                ser.flush()
                sent = True
            n = ser.in_waiting
            if n:
                buf += ser.read(n)
                deadline = max(deadline, time.time() + 0.4)
            else:
                time.sleep(0.02)

        text = buf.decode("utf-8", errors="replace")
        if not text.strip():
            return (f"{dev}: {seconds}s 内没有任何输出。\n"
                    "可能: 板上固件不打印/波特率不对/板子没在跑。")
        return f"--- {dev} @ {baud} ---\n{text}"
    finally:
        ser.close()


@mcp.tool()
def serial_send(port: str = "", commands: str = "", seconds: float = 3.0,
                reset: bool = False, baud: int = DEFAULT_BAUD) -> str:
    """向串口发命令并读回复(默认不复位, 适合和已经在跑的固件交互)。

    commands: 多条用 | 分隔, 例如 "M 1 200|STATUS"
    这是控制电机/读取状态的常用工具。
    """
    return serial_read(port=port, seconds=seconds, reset=reset,
                       send=commands, baud=baud)


@mcp.tool(annotations=READ_ONLY)
def arduino_env() -> str:
    """查看 Arduino 工具链现状(只读): 已装核心、已装库、当前插着的板子。

    排查"为什么编译不过"时先看这个 —— 例如缺核心、库版本不对。
    """
    parts = []
    for label, cmd in (("已安装核心", ["core", "list"]),
                       ("已安装库", ["lib", "list"]),
                       ("当前插着的板子", ["board", "list"])):
        rc, out = _run([ARDUINO_CLI] + cmd + ["--config-file", ARDUINO_CFG], timeout=180)
        parts.append(f"=== {label} ===\n{out.strip()}")
    parts.append(f"=== 路径 ===\narduino-cli: {ARDUINO_CLI}\n配置: {ARDUINO_CFG}\n"
                 f"esptool: {ESPTOOL}\nsketch 根目录: {SKETCH_ROOT}")
    return "\n\n".join(parts)


@mcp.tool()
def arduino_cli(command: str) -> str:
    """直接跑任意 arduino-cli 子命令(逃生舱)。会自动带上 --config-file。

    例: command="core list" / "lib list" / "board listall esp32"
        command="lib install \\"Simple FOC\\""
    """
    args = command.split()
    cmd = [ARDUINO_CLI] + args
    if "--config-file" not in args:
        cmd += ["--config-file", ARDUINO_CFG]
    rc, out = _run(cmd, timeout=1800)
    return f"$ arduino-cli {command}\n(rc={rc})\n{out}"


@mcp.tool()
def esptool(command: str, port: str = "") -> str:
    """直接跑 esptool 子命令(逃生舱)。

    例: command="chip-id" / "flash-id" / "read-mac"
        command="erase-flash"  <-- 危险, 会清空板子
    """
    if not ESPTOOL:
        return "找不到 esptool.exe"
    dev, why = _pick_port(port or None)
    if dev is None:
        return why
    cmd = [ESPTOOL, "--port", dev] + command.split()
    rc, out = _run(cmd, timeout=300)
    return f"$ esptool --port {dev} {command}\n(rc={rc})\n{out}"


# ---------------------------------------------------------------------------
# 自检: 不开 MCP, 直接把每个工具跑一遍 (用于调试)
# ---------------------------------------------------------------------------
def _selftest():
    print("=== list_serial_ports ===");  print(list_serial_ports())
    print("\n=== chip_info ===");         print(chip_info())
    print("\n=== arduino_cli('core list') ==="); print(arduino_cli("core list"))
    print("\n=== serial_read(seconds=3) ===");   print(serial_read(seconds=3))
    print("\n=== esptool('chip-id') ===");       print(esptool("chip-id"))


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        _selftest()
    else:
        _log(f"[bridge] starting; arduino-cli={ARDUINO_CLI}")
        _log(f"[bridge] esptool={ESPTOOL}")
        mcp.run(transport="stdio")
