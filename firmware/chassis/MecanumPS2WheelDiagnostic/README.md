# MecanumPS2WheelDiagnostic：四标签单轮脉冲（历史诊断版）

> **导航**：[仓库总入口](../../../README.md) · [接线与面包板排线](../../../docs/麦轮TB6612接线与面包板排线.md) · [排错索引](../../../docs/diagnostics/2026-10-07/README.md) · [上位机诊断脚本](../../../host/README-diagnostics.md) · [底盘主线固件](../MecanumPS2/README.md) · [诊断固件总表](../MecanumPS2MotionDiagnostic/README.md)

版本 `v1.1+diag-wheels`。**历史固件，仅用于复核对应日志**，不是当前诊断版本。
上电打印 `BOOT MecanumPS2 v1.1+diag-wheels LOCKED`；本版**没有** `INFO` 命令。

## 用途与范围

把上一版（`MecanumPS2Diagnostic`）只支持 RL 的限制放开，`PULSE` 接受 **FL / RL / FR / RR 四个标签**，
用于逐轮对比四个通道。四轮极性仍为 `+1`（尚未装车校准）。

- 硬件：经典 ESP32 + 双 TB6612 两线法。IN 脚 = `16/17、18/19、23/32、15/5`，共用 `STBY=33`。
- PWM 20kHz / 8bit，LEDC 通道 0–7；不加载 SimpleFOC；投掷 14/25/26/27 保持低；AS5600 不使用。
- ⚠️ 极性 `{1,1,1,1}` 是**未装车**默认值，**不要当作装车前进方向**（装车应为 `{1,1,-1,-1}`）。

## 串口命令（115200，换行结束）

| 命令 | 参数与限值 |
|---|---|
| `STOP` / `STATUS` / `PS2` | 锁定 / 状态 / 解锁遥控 |
| `PULSE <轮> duty ms` | FL/RL/FR/RR 四标签均可用；duty 非零 ±1..255；ms=50..500 |
| `JOG <轮> duty ms` | 同上四标签；\|duty\| ≤ 128（`JOG_CAP`）；ms=50..800 |
| PS2 遥控上限 | `CAP=76`（29.8%），L1 + 方向键 |

- **`PULSE` 是固定占空比，不走爬升**；`JOG` 含软启动（每 10ms 变化 ≤3 档），报告里的 duty 是斜坡当前值。
- 单轮 `PULSE` 的 `FL/FR` 指**轮标签**（不是斜行组合 —— 那是 `MOVEPULSE` 的语义，见当前版固件）。

## 安全

- 到期、`STOP`、任意按键、PS2 失帧、非法或重叠命令都会撤销 STBY 并锁定。
- 时限依赖软件循环，另有 1 秒任务看门狗；**都不是独立的硬件电流 / 温度 / 时限保证**。
- 四轮架空、投掷动力断开再点动。停止是**高阻滑行**，不是主动制动。

## 构建

```text
arduino-cli compile --fqbn esp32:esp32:esp32 --build-path build/MecanumPS2WheelDiagnostic firmware/chassis/MecanumPS2WheelDiagnostic
```

本机是便携版 Arduino，需补 `--config-file`。无第三方库依赖；产物留在已忽略的 `build/`。
