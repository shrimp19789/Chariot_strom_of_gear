# ESP32 + 奇果派电机驱动板：链路检测 + 简易电机驱动

一套完整的「开发板 ↔ 电机驱动板」链路检测工具和一个可直接用的电机驱动程序。
**所有固件都已在本机编译验证通过**（ESP32 / ESP32-S3 / ESP32-C3 / UNO / Mega 共 10 种组合）。

> 板子还没插上，所以**烧录和实机验证还没做**。插上后按下面「快速开始」走三步即可。

---

## 结论速览

| 项目 | 状态 | 说明 |
|---|---|---|
| Arduino 工具链 | ✅ 可调用 | 便携版 IDE 2.3.10 @ `D:\Download\arduino`，内置 arduino-cli 1.5.1 |
| ESP32 开发核心 | ✅ 已装 | `esp32:esp32` **3.3.12**，含全套工具链与 esptool 5.3.1 |
| Codex | ✅ 可调用 | `codex-cli 0.160.0` |
| 串口 / 板子 | ⚠️ **未连接** | 见下方说明 |
| 电机驱动板 | ✅ 已识别型号 | 奇果派 QGPMaker 电机驱动板（PCA9685，I2C） |
| 固件编译 | ✅ 全部通过 | 2 个固件 × 5 种开发板 |
| 上位机检测脚本 | ✅ 可用 | `host/motor_link_check.py`（已在无板子情况下验证容错行为） |

### 串口现状

```
COM7 / COM8   蓝牙链接上的标准串行     -> 蓝牙虚拟串口，忽略
COM9          USB-SERIAL CH340        -> 状态 Unknown = 当前未插入
```

`COM9` 就是你的板子：USB 芯片是 **CH340（VID_1A86 : PID_7523）**。
注册表里设备记录还在、CH340 驱动也已装好，只是现在没插上——**和你说的完全一致**。
插上后它会自动以 COM9（或别的编号）出现。

---

## 一、你的硬件到底是什么（挖出来的结论）

Codex 自己的日志里**没有** ESP32 相关内容（`logs_2.sqlite` 只有 1859 行、时间窗 2026-10-05 17:09–18:23，
搜 `esp32 / arduino / motor / 串口 / COM9` 全部 0 命中；`threads` 表为空）。
但真正的线索在本机磁盘上，已经全部找到了：

| 线索 | 位置 | 说明了什么 |
|---|---|---|
| ChatGPT 项目镜像 | `~/.codex/.chatgpt-projects/g-p-.../AGENTS.md` | 有个项目就叫 **“ESP32”** |
| 厂商资料包 | `D:\Download\arduino\_net_test\7gp\v56\` | **奇果派 QGPMaker 电机驱动板**，且厂商**单独提供了 `esp32` 版程序** |
| 驱动库 | `sketchbook\libraries\QGPMakerMotorShield_Lib_V5.2\` | 驱动板 API：4 路直流电机 / 2 路步进 / 8 路舵机 |
| ESP32 核心抓取脚本 | `_net_test\fetch_esp32.ps1` + `_fetch_log.txt` | 曾用 `gh-proxy.com` 加速下载 1.9GB 工具链 |
| 物资表 | `_net_test\wuzi.txt` | 12V 直流电机、**“不用带编码器”**、舵机、麦轮 |
| 驱动安装包 | `arduino\drivers\` | CH341SER + CP210x，驱动已备好 |

厂商产品页（`7gp.cn/archives/308`）确认的关键参数：

- 电机驱动走 **I2C 的 PCA9685**，只占 2 根 IO，**与开发板型号无关** → 所以 ESP32 也能用
- 默认 I2C 地址 **0x60**（V5.6 支持改地址）
- 4 路直流电机 **M1~M4** 或 2 路步进，另有 8 路舵机 PWM
- ⚠️ **编码器功能仅支持 Arduino UNO**，Mega2560 及其他板子都不支持 → 好在你的物资表写明电机“不用带编码器”，正好绕开这个坑

---

## 二、目录结构

```
esp32-motor-shield/
├── README.md                      本文件
├── firmware/
│   ├── MotorLinkCheck/            【固件1】链路自检 / 诊断
│   │   └── MotorLinkCheck.ino
│   └── MotorDriver/               【固件2】简易电机驱动程序
│       └── MotorDriver.ino
├── host/
│   ├── motor_link_check.py        【上位机】串口检测 + 电机控制
│   └── requirements.txt
├── tools/
│   └── build.ps1                  一键编译 / 烧录
└── docs/
    └── 01-环境检查报告.md          本机 Arduino / Codex / 串口 详细检查记录
```

---

## 三、快速开始（板子插上后 3 步）

```powershell
# 第 0 步（一次性）：装上位机依赖
python -m pip install -r D:\dsh\1001\esp32-motor-shield\host\requirements.txt

# 第 1 步：看看板子插上后是哪个串口
D:\dsh\1001\esp32-motor-shield\tools\build.ps1 -ListBoards

# 第 2 步：烧录【链路自检】固件
D:\dsh\1001\esp32-motor-shield\tools\build.ps1 -Sketch MotorLinkCheck -Upload

# 第 3 步：在电脑上跑检测，直接看结论
python D:\dsh\1001\esp32-motor-shield\host\motor_link_check.py
```

第 3 步会打印「**结论: 开发板 <-> 电机驱动板 连接成功 ✔**」或失败原因。

确认通信正常后，再烧电机驱动固件：

```powershell
D:\dsh\1001\esp32-motor-shield\tools\build.ps1 -Sketch MotorDriver -Upload
python D:\dsh\1001\esp32-motor-shield\host\motor_link_check.py --test-motors   # 逐路点动
python D:\dsh\1001\esp32-motor-shield\host\motor_link_check.py --drive 1 200   # M1 正转 200
python D:\dsh\1001\esp32-motor-shield\host\motor_link_check.py --stop
```

> 编译器默认按 **`esp32:esp32:esp32`（ESP32 Dev Module）** 编译，这是通用款，绝大多数 ESP32 板都适用。
> 如果你的板子是别的型号，加 `-Fqbn`：
> `-Fqbn esp32:esp32:esp32doit-devkit-v1`（DOIT DevKit V1）、`-Fqbn esp32:esp32:esp32s3`、`-Fqbn esp32:esp32:esp32c3` …

---

## 四、接线与前提

```
   ┌──────────────┐        ┌────────────────────────┐
   │  ESP32 开发板 │◄──插──► │  奇果派电机驱动板       │
   │  (CH340 USB) │  I2C   │  PCA9685 @ 0x60        │
   └──────────────┘        └────────────────────────┘
        │ USB 供逻辑电              │
        ▼                          ├─ M1~M4 ──► 12V 直流电机 ×4
     电脑 (串口)                    ├─ 舵机口 ──► MG995 等 ×8
                                   └─ DC 口 ◄── 6~12V 动力电池 (VM)
```

- **ESP32 默认 I2C 引脚**：`SDA = GPIO21`、`SCL = GPIO22`（不用改，固件直接用默认）
- **UNO 默认 I2C 引脚**：`SDA = A4`、`SCL = A5`
- ⚠️ **USB 供电不够转电机**。必须接动力电池到驱动板 DC 口（6~12V），
  否则会出现「I2C 检测通过、但电机纹丝不动」——这是最常见的“假故障”。
- 电机转向反了：把该路电机的两根线对调；或在 `MotorDriver.ino` 里改 `MOTOR_INVERT[]`。

---

## 五、固件 1：MotorLinkCheck（链路自检）

上电自动跑一次自检，之后可用串口命令反复检查。

**它比“扫描 I2C 地址”强在哪**：
I2C 扫描只能证明“某个地址上有东西应答”，总线干扰或别的 I2C 器件都可能误判。
本固件会往 PCA9685 的 `MODE1` 寄存器**写已知图案再读回来比对**（自动递增位、SLEEP 位），
写进去什么就读出什么，才判定驱动板真的在线。

**命令**（115200 8N1）：

| 命令 | 作用 |
|---|---|
| `CHECK` | 完整自检并打印报告（开机自动执行） |
| `SCAN` | 只做 I2C 全总线扫描 |
| `TEST ALL` / `TEST <1-4>` | 依次 / 指定点动电机 |
| `M <1-4> <-255..255>` | 手动驱动某路（正=正转，负=反转，0=停） |
| `STOP` | 全部停止 |
| `I2C <sda> <scl>` | 改用自定义 I2C 引脚并重新自检（仅 ESP32 有效） |
| `HELP` | 帮助 |

**报告里会区分三种失败原因**（这才是有用的地方）：

| 现象 | 含义 |
|---|---|
| 扫描到 0 个器件，且**超时**计数 > 0 | 总线被拉死/无上拉 → 驱动板没插好或没供电，或引脚不对 |
| 扫描到 0 个器件，只有 NACK | 总线是好的，但没器件应答 → 驱动板未连接 |
| 扫描到器件但都不是 PCA9685 | 总线上有别的东西，或驱动板地址被改过 |

---

## 六、固件 2：MotorDriver（简易电机驱动）

用**厂家官方库** `QGPMaker_MotorShield` 驱动，支持两套协议。

### ASCII 文本协议（推荐，好调）

| 命令 | 作用 |
|---|---|
| `M <1-4> <-255..255>` | 设置某路电机速度 |
| `STOP` / `BRK <1-4>` | 全部停止 / 单路刹车 |
| `DRIVE <左> <右>` | 差速驱动（左=M1+M2，右=M3+M4） |
| `SERVO <0-7> <0-180>` | 舵机角度 |
| `WD <毫秒>` | 看门狗：超时无命令自动停车（默认 3000ms，`WD 0` 关闭） |
| `PING` / `STATUS` / `HELP` | `PONG` / 状态 JSON / 帮助 |

### 二进制协议（兼容厂家 UartJoystick 例程，14 字节）

```
0x01 0xFF | M1 int16大端 | M2 | M3 | M4 | 舵机1 uint8 | 舵机2 | 舵机3 | 舵机4
```

### 安全设计

- **看门狗**：3 秒没收到命令自动停车，避免电脑/程序崩了小车还在冲
- **舵机归中**：上电先写 90°，防止舵机乱甩
- **驱动板自检**：启动时先探测 I2C，不在线就明确报 `[FAIL]`，而不是让你对着不动的电机猜

---

## 七、上位机工具 `host/motor_link_check.py`

```powershell
python motor_link_check.py                  # 检测链路（最常用）
python motor_link_check.py --list           # 只列串口，标明哪个像开发板
python motor_link_check.py --port COM9      # 指定串口
python motor_link_check.py --test-motors    # 逐路点动 M1~M4
python motor_link_check.py --drive 1 200    # M1 正转 200
python motor_link_check.py --servo 0 90     # 舵机 0 转到 90 度
python motor_link_check.py --stop           # 全部停止
python motor_link_check.py --raw STATUS     # 发任意命令
python motor_link_check.py --json           # 只输出一行 JSON，方便脚本调用
```

它会自动：

1. 按 USB 芯片型号认出哪个串口是开发板（CH340 / CP210x / FTDI / 乐鑫原生 USB），自动忽略蓝牙串口
2. 复位板子抓开机自检，再主动发 `CHECK`
3. **自动判断板上烧的是哪个固件**（MotorLinkCheck / MotorDriver / 不认识的固件）
4. 给出中文结论和排查步骤

退出码：`0` 正常 / `1` 异常 / `2` 找不到串口 —— 可直接用在 CI 或批处理里。

---

## 八、「电机是否接上」到底能测到什么程度（重要，别被忽悠）

必须说清楚一件事：

> **这块驱动板没有任何电流检测电路**，PCA9685 只管发 PWM，H 桥只管放大。
> 所以**纯软件无法感知“电机线圈到底接没接”**——电机接上、没接、线断了，
> 从 I2C 层面看**完全一样**。这是所有“PCA9685 + H 桥”驱动板的共同局限，
> 不是本项目的缺陷。

因此本项目把检测拆成三层，能做到哪层就做到哪层：

| 层级 | 检测内容 | 手段 | 需额外硬件 |
|---|---|---|---|
| **A** | 开发板 ↔ 驱动板 通信 | I2C 扫描 + PCA9685 寄存器回读 | 无 |
| **B** | 电机是否真的转 | `TEST` 逐路点动，人眼/耳朵确认 | 无 |
| **C** | 电机是否真的接上 | 电流采样自动判断 | 需要一个 ACS712 或采样电阻 |

**A 层是硬结论**（写进去读得回来就是通的，可放心依赖）。
**B 层需要你看一眼电机**。
**C 层**已内置，把 `MotorLinkCheck.ino` 顶部改成：

```c
#define SENSE_PIN      34      // ACS712 输出接到 GPIO34
#define SENSE_MV_PER_A 185.0f  // ACS712-05B 是 185mV/A；20A 版填 100，30A 版填 66
```

接好后点动测试会直接打印「电流 xx mA -> 检测到电机 / 疑似未接电机」。
**在此之前，请以 B 层的点动结果为准。**

---

## 九、常见问题

**Q: 编译报 `avr/io.h: No such file or directory`？**
A: 你在给 ESP32 编译厂家那份 `QGPMakerMotorShield_Lib_V5.2` 库。它里面捆了 AVR 专用的
`PinChangeInterrupt.cpp`，ESP32 编不过。本项目已经解决了——见
`docs/01-环境检查报告.md` 的“新增 ESP32 版库”一节。

**Q: 提示串口 `Access is denied` / 拒绝访问？**
A: Arduino IDE 的**串口监视器还开着**占用了串口，关掉再试。

**Q: I2C 检测 PASS，但电机不转？**
A: 十有八九是**没接动力电源（VM）**。I2C 的逻辑电来自开发板 USB，所以通信能通；
但电机要 6~12V。接上电池再试。

**Q: 检测 FAIL，总线上一个器件都没有？**
A: 依次查：驱动板是否插到底 / 插歪；驱动板电源灯是否亮；
烧的是不是本项目固件；实在不行试 `--raw "I2C 21 22"` 或换别的引脚。

**Q: 舵机不动 / 抖动？**
A: 舵机要外部供电。驱动板内嵌 5V/3A 稳压，但大功率舵机（MG995 堵转电流大）
建议拔掉红色跳线帽，用独立电源给舵机供电。

---

## 十、参考

- 驱动板资料（厂商）：<https://www.7gp.cn/archives/308>
- 巡检传感器资料：<https://www.7gp.cn/archives/665>
- 本机检查详情：`docs/01-环境检查报告.md`
- 关于“检测电机是否接上”的社区讨论：
  [Arduino Forum: Checking whether a DC motor is connected or not](https://forum.arduino.cc/t/checking-whether-a-dc-motor-is-connected-or-not/1191441/9)
  · [Is there a way to check if a Motor is connected with Arduino?](https://forum.arduino.cc/t/is-there-a-way-to-check-if-a-motor-is-connected-with-arduino-check-the-circuit/652565/13)
  · [Pololu Dual MC33926 Motor Driver Shield（有电流检测的对照方案）](https://www.pololu.com/docs/0j55/all)
