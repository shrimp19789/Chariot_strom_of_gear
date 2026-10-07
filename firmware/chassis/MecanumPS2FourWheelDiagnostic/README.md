# MecanumPS2FourWheelDiagnostic：四轮同时脉冲（历史诊断版）

> **导航**：[仓库总入口](../../../README.md) · [接线与面包板排线](../../../docs/麦轮TB6612接线与面包板排线.md) · [排错索引](../../../docs/diagnostics/2026-10-07/README.md) · [上位机诊断脚本](../../../host/README-diagnostics.md) · [底盘主线固件](../MecanumPS2/README.md) · [诊断固件总表](../MecanumPS2MotionDiagnostic/README.md)

版本 `v1.1+diag-all`。**历史固件，仅用于复核对应日志** —— 它是**第一版装车校准**（极性 `{1,1,-1,-1}`）
并首次支持四轮同时给占空比。上电打印 `BOOT MecanumPS2 v1.1+diag-all LOCKED`。

## 用途与范围

- 新增 `ALLPULSE`：**四轮同一条命令同时**给固定占空比，用于验证四轮联动。
- 新增 `INFO`：打印固件名、四路极性、`ALLPULSE` 上限与时限。
- 极性改为 `{1,1,-1,-1}`（源码注释：**已在装车状态核实**），四路顺序 FL,RL,FR,RR。

- 硬件：经典 ESP32 + 双 TB6612 两线法。IN 脚 = `16/17、18/19、23/32、15/5`，共用 `STBY=33`。
- PWM 20kHz / 8bit，LEDC 通道 0–7；不加载 SimpleFOC；投掷 14/25/26/27 保持低；AS5600 不使用。

## 串口命令（115200，换行结束）

| 命令 | 参数与限值 |
|---|---|
| `STOP` / `STATUS` / `INFO` / `PS2` | 锁定 / 状态 / 版本与限值 / 解锁遥控 |
| `ALLPULSE duty ms` | duty 非零 **±1..192**（`ALL_PULSE_CAP=192`，75.3%）；ms=**50..300**（`ALL_PULSE_MAX_MS`） |
| `PULSE <轮> duty ms` | FL/RL/FR/RR；duty 非零 ±1..255；ms=50..500 |
| `JOG <轮> duty ms` | 同上；\|duty\| ≤ 128（`JOG_CAP`）；ms=50..800 |
| PS2 遥控上限 | `CAP=76`（29.8%） |

- **`ALLPULSE` / `PULSE` 是固定占空比，不走爬升**；`JOG` 含软启动。
- `ALLPULSE` 要求**四轮归零至少 300ms** 才接受，避免和上一次动作叠加。

## 安全

- ⚠️ **`ALLPULSE` 一趟就同时给四个轮子加电**，是四轮联动排查的最小工具；但 192/255 已是 75% 占空比，
  **只在架空、VM 受限、投掷动力断开时用**。四轮同时启动的电流是单轮的四倍，先确认电源限流。
- 到期、`STOP`、任意按键、PS2 失帧、非法或重叠命令都会撤销 STBY 并锁定。
- 时限依赖软件循环，另有 1 秒任务看门狗；**都不是独立的硬件电流 / 温度 / 时限保证**。
- ⚠️ 本版**没有**持续联动能力（`ALLPULSE` 最长 300ms）——「四轮持续联动不通过」这个问题要到
  当前版 [MecanumPS2MotionDiagnostic](../MecanumPS2MotionDiagnostic/README.md) 的 `MOVEPULSE` 才覆盖。

## 构建

```text
arduino-cli compile --fqbn esp32:esp32:esp32 --build-path build/MecanumPS2FourWheelDiagnostic firmware/chassis/MecanumPS2FourWheelDiagnostic
```

本机是便携版 Arduino，需补 `--config-file`。无第三方库依赖；产物留在已忽略的 `build/`。
