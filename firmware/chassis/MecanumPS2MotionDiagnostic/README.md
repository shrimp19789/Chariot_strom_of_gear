# 两线法麦轮限时诊断固件

> **导航**：[仓库总入口](../../../README.md) · [接线与面包板排线](../../../docs/麦轮TB6612接线与面包板排线.md) · [排错索引](../../../docs/diagnostics/2026-10-07/README.md) · [底盘主线固件](../MecanumPS2/README.md) · [第2步逻辑验收](../TB6612LogicCheck/README.md) · [上位机诊断脚本](../../../host/README-diagnostics.md)

当前版本 `MecanumPS2-v1.1+diag-motion`。用于架空诊断，历史实测结论见 [2026-10-07 排错索引](../../../docs/diagnostics/2026-10-07/README.md)。原归档中持续四轮联动未通过；后续复测已获用户确认四轮逐轮双向 2 秒、四轮同时前进/后退各 2 秒通过，见 [本轮记录](../../../docs/logs/2026-10-07-retest/重新烧录与四轮同时正反2秒.md)。原故障原因仍未定位。

2026-10-07 复测扩展：`PULSEINFO` 返回 `revision=single-2s maxDuty=255 maxMs=2000`，单轮 PULSE 上限从 500ms 改为 2000ms；历史归档日志对应原 500ms 版本。INFO 保持兼容，新增能力由 PULSEINFO 单独辨认。四轮 ALLPULSE 上限仍为 300ms。

随后新增 `RAMPINFO` / `RAMPPULSE`，仅支持前进和后退，1000..10000ms、峰值占空比 1..192。前半程从零逐步增加到峰值，后半程逐步降到零；无转速反馈，不能据此声称速度随占空比线性变化。原 MOVEPULSE、PULSE 和 ALLPULSE 的限值与方向保留。

本轮已执行前进/后退渐变、左移/右移组合各 10 秒，串口检查通过，实物结果待现场回填，见 [渐变与左右组合记录](../../../docs/logs/2026-10-07-retest/四轮渐变与左右平移10秒.md)。

为用户要求的胶带计圈与电压测量，固定占空比 `MOVEPULSE FWD` 单独扩展至 30000ms；`FORWARDINFO` 返回 `revision=forward-30s maxDuty=192 maxMs=30000`。其他运动组合及渐变上限保留为 10000ms。

基于原 `MecanumPS2` 的提交 `c76249435cf63825123c24b832abb2f966b35686`。IN GPIO、两线法、PS2、20kHz/8bit PWM、原 PS2 上限 76 与 JOG 上限 128 保留；新增固定占空比命令不走原爬升。装车极性为 `{1,1,-1,-1}`，顺序 FL,RL,FR,RR。投掷 EN/相线保持低，AS5600 未使用。

## 命令及版本

| 源码目录 | 版本 | 诊断范围 | 极性 |
| --- | --- | --- | --- |
| `../MecanumPS2Diagnostic/` | `v1.1+diag-pulse` | 历史 RL-only，PULSE 最大 500ms | 四路均 +1 |
| `../MecanumPS2WheelDiagnostic/` | `v1.1+diag-wheels` | 历史四标签单轮 PULSE 最大 500ms | 四路均 +1 |
| `../MecanumPS2FourWheelDiagnostic/` | `v1.1+diag-all` | 历史装车校准，ALLPULSE 最大 300ms | +1,+1,-1,-1 |
| 本目录 | `v1.1+diag-motion` | 当前版，增加十种 MOVEPULSE | +1,+1,-1,-1 |

历史源码用于复核对应日志；不要把旧版本方向定义当作装车前进方向。
前两个历史版**没有** `INFO` 命令，上电只打印 `BOOT MecanumPS2 v1.1+diag` / `v1.1+diag-wheels`；`-pulse`、`-wheels` 是本归档为区分阶段加的标注，也是配套主机脚本的配对依据（见 [host/README-diagnostics.md](../../../host/README-diagnostics.md)）。

| 命令 | 参数 |
| --- | --- |
| `STOP` / `STATUS` / `INFO` / `MOTIONINFO` | 停止、状态与版本/限值 |
| `PULSE FL\|RL\|FR\|RR duty ms` | duty 为非零 ±1..255；ms=50..2000 |
| `PULSEINFO` | 单轮诊断修订及占空比、时限上限 |
| `ALLPULSE duty ms` | duty 为非零 ±1..192；ms=50..300 |
| `MOVEPULSE name duty ms` | name=FWD/BACK/LEFT/RIGHT/FL/FR/BL/BR/CCW/CW；duty=1..192；ms=50..10000，FWD 单独允许至 30000 |
| `FORWARDINFO` | `revision=forward-30s maxDuty=192 maxMs=30000` |
| `RAMPPULSE FWD\|BACK peakDuty ms` | peakDuty=1..192；ms=1000..10000；前半程升、后半程降 |
| `RAMPINFO` | `revision=triangle-v1 maxDuty=192 minMs=1000 maxMs=10000` |

单轮 PULSE 的 FL/FR 指轮标签；MOVEPULSE 的 FL/FR 指左前/右前斜行组合。命令符号按校准后的车辆逻辑方向换算，串口 `duty` 显示实际 GPIO 方向符号。例如前进 192 的报告为 `192,192,-192,-192`。

启用要求硬件正常、PS2 帧健康且所有按键松开、控制锁定且无其他点动；ALL/MOVE/RAMP 还要求四轮归零至少 300ms。到期、STOP、按键输入、PS2 失效和非法/重叠命令均撤销使能并锁定。时限依赖软件循环，保留 1 秒看门狗，不是独立硬件电流、温度或时限保证。

## 构建与使用

安装 Arduino CLI 和 `esp32:esp32` 核心 3.3.12，在仓库根目录执行仅编译：

```text
arduino-cli compile --fqbn esp32:esp32:esp32 --build-path build/MecanumPS2MotionDiagnostic firmware/chassis/MecanumPS2MotionDiagnostic
```

若使用自定义 Arduino 数据目录，应按本机配置补 `--config-file`；Windows 工具链若不支持中文构建路径，使用英文临时构建路径。程序没有额外 Arduino 第三方库依赖。编译产物留在忽略目录，不提交镜像。

烧录与 VM-off 检查时须实体关闭动力，仅保留 USB 逻辑供电。带动力动作前确认四轮架空、VM=6V、手柄松键与待机不动。串口 115200，主机脚本见 [host 说明](../../../host/README-diagnostics.md)。当前不安排十种带动力组合连续重测；先完成待测输出电压诊断。

## 视频圈数占空比补偿

2026-10-07 根据 30 秒视频 32/33/27/24 rpm，增加 `COMP ON`、`COMP OFF`、`COMPINFO`。采用初始目标 30 rpm，基准 192 的四路逻辑 PWM 为 180/175/213/240；输出方向仍由装车极性决定。只在闲置并满足归零间隔时切换。补偿作用于 MOVEPULSE、RAMPPULSE、PS2，PULSE/ALLPULSE/JOG 保持原始值；重启默认关闭，STOP 停止输出但不切换补偿模式。

主机使用 `--compensate` 开启并核查精确配置，结束后关闭补偿。此为前进架空单点开环估计，实际轮速、反转及负载变化须复测。详见 [参数、验证与待测记录](../../../docs/logs/2026-10-07-retest/占空比补偿-30秒视频校准.md)。

历史修订 `video30s-v2`：v1 带动力短测只有右侧轮转，且右侧仍不同速；v2 将左侧恢复为原始占空比，基准 192 对应逻辑 PWM 为 192/192/213/240，随后左前仍不动、左后明显慢。两版均未通过实物验收，保留过程于测试记录。

当前修订为 `start30s-v3`，增益 FL/RL/FR/RR=`76/119,76/135,76/101,1/1`，输出上限 192，基准 192 对应逻辑 PWM `123,108,144,192`。数据取自统一 255 起转 300ms、192 维持的 30 秒视频：14.875、16.875、12.625、9.5 圈。**v3 实物测试失败：只有右后持续转到停止提示，其他三轮提前停转。** 系数仅用于复现失败诊断，不能当作已校准的同速方案；补偿默认关闭。

## 单轮起转与维持诊断

新增显式命令 `STARTPULSE wheel signedHoldDuty totalMs`：单个轮子先以同符号 255 输出 300ms，再降至指定非零 ±1..255 维持值；总时长 500..2000ms 包含起转阶段。`STARTPULSEINFO` 返回 `revision=kick300-v1 startDuty=255 startMs=300 maxDuty=255 minMs=500 maxMs=2000`。状态标记为 STARTPULSE，报告实际 GPIO 方向；RR +192 的前 300ms 为 -255，此后为 -192。

要求健康 PS2、松键、锁定且四轮归零满 300ms；STOP、非法/重叠命令、失联、按键及超时撤销输出并清除起转模式。此命令独立于补偿系数；不自动用于 PS2 或其他运动指令。它只用于验证起转后较低占空比能否保持运动，不代表转速补偿完成。

## 四轮前进起转与维持诊断

新增 `STARTMOVE FWD holdDuty totalMs`，仅前进、维持值 1..192、总时长 500..30000ms。前 300ms 四轮按装车极性输出 255，之后回到逻辑维持值；若显式 COMP ON，维持阶段按已有系数换算。串口能力 `STARTMOVEINFO revision=kick300-fwd-v1 startDuty=255 startMs=300 maxDuty=192 minMs=500 maxMs=30000`，状态 STARTMOVE。需健康 PS2、松键、锁定且四轮归零满 300ms，沿用 STOP、到期及错误互锁。原 ALLPULSE 的 192/300ms 上限不变。

主机 `pulse_once.py --wheel ALL --startup` 默认强制并核查 COMP OFF，测试原始前进起转；显式加入 `--compensate` 才启用补偿维持，并在结束时关闭补偿。统一 192 维持的带动力短测用户回报四轮都可动，30 秒圈数已记录；v3 补偿维持未通过。最新单轮左后 180 短测已完成软件阶段和停止检查，现场持续转动结果待回填。详见补偿记录。
