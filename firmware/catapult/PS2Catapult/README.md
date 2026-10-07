# PS2Catapult：投掷机构主固件（PS2 遥控 + 蓄力 + 90° 往返）

> **导航**：[仓库总入口](../../../README.md) · [ESP32-2804 接线与烧录指南](../../../docs/ESP32-2804-FOC-guide.md) · [投掷机构-设计与安全](../../../docs/投掷机构-设计与安全.md) · [PS2遥控实现与调试](../../../docs/PS2遥控实现与调试.md) · [阻塞问题与烧录实测](../../../docs/阻塞问题与烧录实测.md) · [Stage1](../Stage1_AS5600Test/README.md) · [Stage2](../Stage2_FOCClosedLoop/README.md)

**投掷机构的当前主固件。** 上一版记录：已上传 COM9 并验证启动（332524 字节 / 25%，全局变量 24548 / 7%）。
默认**锁定**：`CONFIG_CONFIRMED=false`、`SUPPLY_VOLTAGE=0`，驱动 EN 保持低电平，
上电只打印状态、不驱动电机。

## 引脚

| 信号 | GPIO | 信号 | GPIO |
|---|---:|---|---:|
| AS5600 SDA / SCL | 21 / 22 | PS2 CLK / CS | 2 / 4 |
| DRV8313 IN1 / IN2 / IN3 | 25 / 26 / 27 | PS2 CMD / DAT | 12 / 13 |
| DRV8313 EN | 14 | DRV8313 FLT | 不接（开漏，厂家注明不接） |

> ⚠️ **不要把 PS2 改到 18/23 避开 GPIO2/12** —— 那两个脚现在是底盘电机线
> （18 = 左后 BIN1、23 = 右前 AIN1，见 [接线文档](../../../docs/麦轮TB6612接线与面包板排线.md)）。
> 若确需避开，必须与底盘引脚表整体重排。

## 必须人工核对的配置（源码顶部）

| 常量 | 默认 | 说明 |
|---|---|---|
| `CONFIG_CONFIRMED` | `false` | 改成 `true` 才会使能电机 |
| `SUPPLY_VOLTAGE` | `0.0f` | 填**驱动板功率输入实测值**，不能填逻辑 5V |
| `POLE_PAIRS` | `7` | 2804 是 12N14P → 7 对极 |
| `DIRECTION` | `1.0f` | 只允许 ±1，卸杆低速核对正方向 |
| `VOLTAGE_LIMIT` | `1.0f` | **不得靠提高此值解决"电机不转"** |

## 串口命令（115200）

| 键 | 作用 |
|---|---|
| `c` | FOC 对齐 / 校准 —— **电机会动**（卸杆后做，阻塞期间软件急停不保证响应） |
| `h` | 记录当前编码器角度为 0 度（**人工置零**，不是自动找限位） |
| `a` | 使能（需已校准 + 已置零 + PS2 帧健康） |
| `x` | 立即软件禁用 |
| `d` | 编码器诊断（仅电机禁用时可用；查 I2C 引脚电平、总线恢复、低速复读） |

状态机：`LOCKED → UNCALIBRATED / DISARMED → READY → CHARGING → FORWARD → DWELL(300ms) → RETURNING → READY`；
任一故障源进入 `FAULT`。**故障时不自动回零。**

## 安全红线

- **软件停止不能替代断开驱动功率电源。** `VOLTAGE_LIMIT=1.0V` 是因为这颗电机堵转电流 ≈ 电压 ÷ Rs(2.3Ω)：
  1V ≈ 0.43A（安全），12V ≈ **5.2A**（远超 2A 上限，37g 电机会烧）。
- **没有电流采样**，是电压模式 FOC，**软件无法限制真实电流**。
- 禁用后机构可能惯性运动或受重力下落 —— 必须有**独立硬限位与物理急停**。
- 首次测试**不装投掷杆**。

首次调试顺序见 [README §5](../../../README.md)。

## 构建 / 烧录

```text
arduino-cli compile --fqbn esp32:esp32:esp32 --build-path build/PS2Catapult firmware/catapult/PS2Catapult
```

> ⚠️ **`tools/flash-catapult.ps1` 目前只支持 `-Stage 1|2`，不认识本固件** —— 烧它需手动调用
> arduino-cli（或扩展该脚本的 `-Stage`）。依赖 `Simple FOC 2.4.0` 与 `esp32:esp32` 3.3.12。
