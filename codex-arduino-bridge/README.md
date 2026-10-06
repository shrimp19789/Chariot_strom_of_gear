# codex-arduino-bridge

让 **Codex CLI 直接操作本机的串口和 Arduino 工具链** 的 MCP 服务器。

现在 Codex 可以自己完成这一整条链：

```
看板子插在哪个口 → 问芯片是什么 → 编译 → 烧录 → 复位读串口 → 发命令交互
```

---

## 一、它是怎么接上的

```
┌──────────────┐   MCP(JSON-RPC over stdio)   ┌─────────────────────┐
│  Codex CLI   │ ◄──────────────────────────► │ mcp_server.py       │
│  (0.160.0)   │                              │ 9 个工具            │
└──────────────┘                              └──────────┬──────────┘
                                                         │
                                    ┌────────────────────┼────────────────────┐
                                    ▼                    ▼                    ▼
                              pyserial            arduino-cli 1.5.1      esptool 5.3.1
                              (COM 口)            (编译/烧录)            (认芯片)
```

注册信息写在 `~/.codex/config.toml` 的 `[mcp_servers.arduino]`：

```toml
[mcp_servers.arduino]
command = 'D:\Download\Python\python.exe'
args = ['D:\dsh\1001\codex-arduino-bridge\mcp_server.py']

[mcp_servers.arduino.env]
PYTHONUTF8 = "1"          # 关键: 否则 Windows 下中文会乱码
```

---

## 二、9 个工具，以及"要不要审批"

这是本桥最需要理解的一点 —— **审批是分级的，不是一刀切**。

| 工具 | 作用 | 注解 | Codex 要不要审批 |
|---|---|---|---|
| `list_serial_ports` | 列串口，标出 USB 芯片，排除蓝牙口 | `read_only` | **免审批** |
| `chip_info` | esptool 读芯片/Flash/MAC，给建议 FQBN | `read_only` | **免审批** |
| `arduino_env` | 看已装核心/库/当前板子 | `read_only` | **免审批** |
| `arduino_compile` | 编译 sketch | — | 需审批 |
| `arduino_upload` | 编译并烧录（**覆盖板上固件**） | — | 需审批 |
| `serial_read` | 复位并读串口，可发命令 | — | 需审批 |
| `serial_send` | 向串口发命令（**可能让电机转起来**） | — | 需审批 |
| `arduino_cli` | 任意 arduino-cli 子命令（逃生舱） | — | 需审批 |
| `esptool` | 任意 esptool 子命令（逃生舱，含 erase-flash） | — | 需审批 |

设计意图：**侦查类零摩擦，改硬件状态一律要你点头。**
尤其是 `serial_send` —— 这台设备上挂着 2804 无刷电机，一条串口命令就能让它转起来，
所以它**刻意不标 read-only**。

> 在交互式 Codex 里，遇到需审批的工具它会弹出确认，你可以选择"始终允许该工具"来减少打扰。
> 也可以用下面「六、想放宽/收紧」里的办法改。

---

## 三、怎么用

先起一个 Codex 会话：

```powershell
& "C:\Users\jiahe\AppData\Local\OpenAI\Codex\bin\f544b3844e0f14e9\codex.exe"
```

然后直接说人话就行，例如：

- 「列一下串口，看板子插在哪个口」→ 自动调 `list_serial_ports`
- 「这块板子是什么芯片，该用哪个 FQBN」→ 自动调 `chip_info`
- 「编译 ESP小车项目 的 Stage1，然后烧进去」→ `arduino_compile` + `arduino_upload`
- 「复位板子，把串口输出前 30 行给我」→ `serial_read`
- 「往串口发 STATUS 看看电机状态」→ `serial_send`

---

## 四、已经验证到什么程度

不是"配置写好了"就算了，是真跑通了：

**1. MCP 协议层**（不经过 Codex，直接讲 JSON-RPC）

```
1) initialize ...   OK serverInfo={'name':'arduino-serial-bridge','version':'1.0.0'}
                          protocol=2025-06-18
2) tools/list ...   OK 共 9 个工具
3) tools/call list_serial_ports ...  OK
4) tools/call chip_info ...          OK  (ESP32-D0WD-V3, 4MB, MAC 08:a6:f7:a8:95:94)
```

复现：`python test_mcp.py`

**2. Codex 端到端**（`codex exec`，真实模型调用）

第一次测试暴露了审批问题，正好印证了注解的作用：

```
mcp_tool_call server=arduino tool=list_serial_ports
error: "MCP tool call requires approval, but approval policy is never"   ← 未加注解时
```

加上 `read_only_hint` 之后：

```
mcp_tool_call server=arduino tool=list_serial_ports ... status=completed
→ "共 3 个串口: COM9 USB-SERIAL CH340 <== 最像开发板 ..."      ← 免审批直接跑通
```

串口也能读（这条走了审批绕过，因为它按设计需要审批）：

```
mcp_tool_call server=arduino tool=serial_read arguments={"seconds":4,"reset":true}
→ "--- COM9 @ 115200 --- ets Jul 29 2019 ... rst:0x1 (POWERON_RESET) ..."
```

中文全程正常（靠 `PYTHONUTF8=1`）。

---

## 五、管理这个 MCP 服务器

```powershell
$cx = "C:\Users\jiahe\AppData\Local\OpenAI\Codex\bin\f544b3844e0f14e9\codex.exe"

& $cx mcp list              # 看所有 MCP 服务器
& $cx mcp get arduino       # 看这个的配置
& $cx mcp remove arduino    # 卸载

# 重新注册
& $cx mcp add arduino --env PYTHONUTF8=1 -- `
      "D:\Download\Python\python.exe" "D:\dsh\1001\codex-arduino-bridge\mcp_server.py"
```

改过 `config.toml`，原始备份在：

```
~/.codex/config.toml.bak-before-arduino
```

单独调试服务器（不开 Codex）：

```powershell
python mcp_server.py --selftest    # 把每个工具本地跑一遍
python test_mcp.py                 # 验证 MCP 协议
```

---

## 六、想放宽 / 收紧

**放宽**（让某个工具也免审批）：在 `mcp_server.py` 里给它加 `annotations=READ_ONLY`
或自定义 `ToolAnnotations(...)`，然后重启 Codex 会话。

**收紧**：把 `READ_ONLY` 从 `list_serial_ports` / `chip_info` / `arduino_env` 上摘掉。

**换串口/换板子**：`serial_*` 和 `chip_info` 都支持显式 `port` 参数；
不传则自动挑，且**认不出 USB 芯片就报错让你指定，绝不乱选**（蓝牙口会被主动排除）。

**换 Arduino 安装位置 / 工程根目录**：用环境变量覆盖，不必改代码

```powershell
& $cx mcp add arduino --env PYTHONUTF8=1 `
      --env ARDUINO_PORTABLE_HOME=D:\Download\arduino `
      --env ARDUINO_SKETCH_ROOT=D:\dsh\1001 `
      -- "D:\Download\Python\python.exe" "D:\dsh\1001\codex-arduino-bridge\mcp_server.py"
```

---

## 七、已知约束

| 约束 | 说明 |
|---|---|
| 串口独占 | 同一时刻只能有一个进程占用 COM 口。Arduino IDE 的串口监视器开着时，工具会报"打不开"并提示你去关掉 |
| 需要 pyserial | `python -m pip install pyserial`（本机已装 3.5） |
| 需要 mcp SDK | `python -m pip install mcp`（本机已装 2.3.0；用了 v2 的 `MCPServer` API） |
| 路径是硬编码默认值 | 与本机现状一致，但可用环境变量覆盖（见上） |
| `arduino-cli` 必须带 `--config-file` | 本机是便携安装，数据目录在 D 盘，不带参数会找不到 esp32 核心。桥里已自动带上 |
| 新增工具后要重启 Codex | MCP 服务器在会话启动时拉起，改代码后需要新开会话 |

---

## 八、和项目脚本的关系

桥是**通用的**（任何 sketch 都能编译烧录），和 `ESP小车项目` 解耦。
项目专用的诊断脚本（`i2c_pin_sweep.py`、`serial_watch.py`）没有被包进 MCP ——
Codex 需要时可以直接用 shell 跑它们；如果希望它们也成为 MCP 工具，在这里加几个
`@mcp.tool()` 包装即可。
