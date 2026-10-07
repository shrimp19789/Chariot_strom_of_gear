# 两线法麦轮限时诊断固件

当前版本 `MecanumPS2-v1.1+diag-motion`。用于架空诊断，现场实测结论见 [2026-10-07 排错索引](../../../docs/diagnostics/2026-10-07/README.md)。单轮和四轮 300ms 短测可转，持续四轮联动未通过。

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
| `PULSE FL\|RL\|FR\|RR duty ms` | duty 为非零 ±1..255；ms=50..500 |
| `ALLPULSE duty ms` | duty 为非零 ±1..192；ms=50..300 |
| `MOVEPULSE name duty ms` | name=FWD/BACK/LEFT/RIGHT/FL/FR/BL/BR/CCW/CW；duty=1..192；ms=50..10000 |

单轮 PULSE 的 FL/FR 指轮标签；MOVEPULSE 的 FL/FR 指左前/右前斜行组合。命令符号按校准后的车辆逻辑方向换算，串口 `duty` 显示实际 GPIO 方向符号。例如前进 192 的报告为 `192,192,-192,-192`。

启用要求硬件正常、PS2 帧健康且所有按键松开、控制锁定且无其他点动；ALL/MOVE 还要求四轮归零至少 300ms。到期、STOP、按键输入、PS2 失效和非法/重叠命令均撤销使能并锁定。时限依赖软件循环，保留 1 秒看门狗，不是独立硬件电流、温度或时限保证。

## 构建与使用

安装 Arduino CLI 和 `esp32:esp32` 核心 3.3.12，在仓库根目录执行仅编译：

```text
arduino-cli compile --fqbn esp32:esp32:esp32 --build-path build/MecanumPS2MotionDiagnostic firmware/chassis/MecanumPS2MotionDiagnostic
```

若使用自定义 Arduino 数据目录，应按本机配置补 `--config-file`；Windows 工具链若不支持中文构建路径，使用英文临时构建路径。程序没有额外 Arduino 第三方库依赖。编译产物留在忽略目录，不提交镜像。

烧录与 VM-off 检查时须实体关闭动力，仅保留 USB 逻辑供电。带动力动作前确认四轮架空、VM=6V、手柄松键与待机不动。串口 115200，主机脚本见 [host 说明](../../../host/README-diagnostics.md)。当前不安排十种带动力组合连续重测；先完成待测输出电压诊断。
