# MotorDriver：奇果派 PCA9685 简易电机驱动（固件 2）

> **导航**：[仓库总入口](../../../README.md) · [奇果派方案详解](../../../docs/底盘链路检测与驱动.md) · [⚠️ 底盘已改用 TB6612](../../../docs/麦轮TB6612接线与面包板排线.md) · [两线法取舍论证](../../../docs/麦轮TB6612接线-两线法备选方案.md) · [底盘主线固件](../MecanumPS2/README.md)

> ⚠️ **本固件只适用奇果派 QGPMaker 驱动板（PCA9685 @ I2C `0x60`），烧到 TB6612 上不会有任何反应。**
> 用它跑麦轮还有一个**结构性限制**：`DRIVE` 是**差速分组**（左 = M1+M2，右 = M3+M4），
> 而麦轮横移要求**四轮独立速度**，差速分组做不出横移。底盘当前方案见
> [2 × TB6612FNG 两线法](../../../docs/麦轮TB6612接线与面包板排线.md)。

用**厂家官方库** `QGPMaker_MotorShield` 驱动，支持两套协议。

## ASCII 文本协议（推荐，好调）

| 命令 | 作用 |
|---|---|
| `M <1-4> <-255..255>` | 设置某路电机速度 |
| `STOP` / `BRK <1-4>` | 全部停止 / 单路刹车 |
| `DRIVE <左> <右>` | 差速驱动（左=M1+M2，右=M3+M4） |
| `SERVO <0-7> <0-180>` | 舵机角度（本板最多 8 路） |
| `WD <毫秒>` | 看门狗：超时无命令自动停车（默认 3000ms，`WD 0` 关闭） |
| `PING` / `STATUS` / `HELP` | `PONG` / 状态 JSON / 帮助 |

## 二进制协议（兼容厂家 UartJoystick 例程，14 字节）

```
0x01 0xFF | M1 int16大端 | M2 | M3 | M4 | 舵机1 uint8 | 舵机2 | 舵机3 | 舵机4
```

## 安全设计

- **看门狗**：3 秒没收到命令自动停车，避免电脑/程序崩了小车还在冲。
- **舵机归中**：上电先写 90°，防止舵机乱甩。
- **驱动板自检**：启动时先探测 I2C，不在线就明确报 `[FAIL]`，而不是让你对着不动的电机猜。
- 电机转向反了：把该路电机两根线对调，或改源码里的 `MOTOR_INVERT[]`。

## 上位机

```powershell
python host\motor_link_check.py --test-motors    # 逐路点动 M1~M4
python host\motor_link_check.py --drive 1 200    # M1 正转 200
python host\motor_link_check.py --stop
```

## 构建 / 烧录

```powershell
.\tools\build-chassis.ps1 -Sketch MotorDriver -Upload
```

**依赖厂家库 `QGPMaker_MotorShield`**（注意：厂家原库捆了 AVR 专用的 `PinChangeInterrupt.cpp`，
ESP32 编不过；本项目用的是已修好的 ESP32 版，见 [环境检查报告](../../../docs/环境检查报告.md)）。
