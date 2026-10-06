#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""不依赖 Codex, 直接跟 MCP 服务器讲 JSON-RPC, 验证协议是否正常。

MCP 的 stdio 传输就是"一行一个 JSON-RPC 消息"。
"""
import json
import os
import subprocess
import sys
import threading
import time

# 与本文件同目录的 MCP 服务器, 不写死绝对路径
SERVER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "mcp_server.py")


def reader(proc, sink):
    for raw in proc.stdout:
        line = raw.decode("utf-8", errors="replace").strip()
        if line:
            sink.append(line)


def main():
    proc = subprocess.Popen(
        [sys.executable, SERVER],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    out = []
    t = threading.Thread(target=reader, args=(proc, out), daemon=True)
    t.start()

    def send(obj):
        proc.stdin.write((json.dumps(obj) + "\n").encode())
        proc.stdin.flush()

    def wait_for(msg_id, timeout=90):
        end = time.time() + timeout
        while time.time() < end:
            for line in list(out):
                try:
                    m = json.loads(line)
                except Exception:
                    continue
                if m.get("id") == msg_id:
                    return m
            time.sleep(0.05)
        return None

    print("1) initialize ...")
    send({"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {
        "protocolVersion": "2025-06-18",
        "capabilities": {},
        "clientInfo": {"name": "bridge-test", "version": "1.0"},
    }})
    r = wait_for(1)
    if not r:
        print("   !! initialize 没有回应")
        print("   stderr:", proc.stderr.read(4000).decode("utf-8", "replace"))
        proc.kill(); return 1
    info = r.get("result", {}).get("serverInfo", {})
    print(f"   OK serverInfo={info} protocol={r.get('result',{}).get('protocolVersion')}")

    send({"jsonrpc": "2.0", "method": "notifications/initialized"})

    print("2) tools/list ...")
    send({"jsonrpc": "2.0", "id": 2, "method": "tools/list"})
    r = wait_for(2)
    if not r:
        print("   !! tools/list 没有回应"); proc.kill(); return 1
    tools = r.get("result", {}).get("tools", [])
    print(f"   OK 共 {len(tools)} 个工具:")
    for tl in tools:
        print(f"      - {tl['name']}: {tl.get('description','')[:60]}")

    print("3) tools/call list_serial_ports ...")
    send({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
          "params": {"name": "list_serial_ports", "arguments": {}}})
    r = wait_for(3)
    if not r:
        print("   !! tools/call 没有回应"); proc.kill(); return 1
    res = r.get("result", {})
    if r.get("error"):
        print("   !! 错误:", r["error"]); proc.kill(); return 1
    content = res.get("content", [])
    text = content[0].get("text", "") if content else json.dumps(res)
    print("   OK 返回内容:")
    for line in text.splitlines():
        print("      " + line)

    print("4) tools/call chip_info ...")
    send({"jsonrpc": "2.0", "id": 4, "method": "tools/call",
          "params": {"name": "chip_info", "arguments": {}}})
    r = wait_for(4)
    if r and not r.get("error"):
        content = r.get("result", {}).get("content", [])
        text = content[0].get("text", "") if content else ""
        print("   OK 返回内容:")
        for line in text.splitlines():
            print("      " + line)
    else:
        print("   !! 失败:", r)

    proc.kill()
    print("\n协议验证通过。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
