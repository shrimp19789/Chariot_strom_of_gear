# TB6612 麦轮诊断上位机脚本

本目录下 2026-10-07 新增的 6 个脚本，用于**架空、限时**的 TB6612 四轮链路诊断。

配套归档与现场结论：[`docs/diagnostics/2026-10-07/README.md`](../docs/diagnostics/2026-10-07/README.md)。
固件命令表与限值：[`firmware/chassis/MecanumPS2MotionDiagnostic/README.md`](../firmware/chassis/MecanumPS2MotionDiagnostic/README.md)。

> ⚠️ **这些脚本只核对串口状态与固件自报的时限，不测量电流、电压、转速或真实轮动。**
> 日志里的 `PASS` 只表示串口检查通过，**不能当作电机验收通过**。
> 截至归档时，四轮持续联动**仍未通过**，根因未定位。

---

## 1. 前置条件

| 项目 | 要求 |
| --- | --- |
| Python | 3.8+，依赖 `pyserial>=3.5`：`python -m pip install -r host/requirements.txt` |
| 串口 | 电脑上**恰好一个** CH340（VID:PID = `1A86:7523`）。脚本按 VID/PID 过滤，缺一个或多一个都会直接抛错 |
| 波特率 | 115200 |
| 端口占用 | 运行前关闭 Arduino IDE 串口监视器、`serial_watch.py` 等占用串口的程序 |
| 固件 | 板上已烧入 §3 表中**配对版本**的诊断固件 |

脚本不接收 `--port`，自己按 VID/PID 找端口；打开串口前先把 `DTR`/`RTS` 置为 `False`，
避免打开串口时把 ESP32 拉进复位/下载态。蓝牙虚拟串口不会被选中，因为 VID/PID 不匹配。

## 2. 日志写到哪里

| 脚本 | 记录位置 |
| --- | --- |
| [`motion_sequence.py`](motion_sequence.py)、[`pulse_once.py`](pulse_once.py) | `--log` 显式指定的路径，不限目录 |
| [`verify_pulse_vm_off.py`](verify_pulse_vm_off.py) | `<仓库根>\logs\诊断程序-VM关闭验证-2026-10-07.txt` |
| [`verify_wheel_pulse_vm_off.py`](verify_wheel_pulse_vm_off.py) | `<仓库根>\logs\四通道诊断程序-VM关闭验证-2026-10-07.txt` |
| [`verify_all_pulse_vm_off.py`](verify_all_pulse_vm_off.py) | `<仓库根>\logs\四轮联动诊断-VM关闭验证-2026-10-07.txt` |
| [`verify_motion_vm_off.py`](verify_motion_vm_off.py) | `<仓库根>\logs\麦轮运动组合诊断-VM关闭验证-2026-10-07.txt` |

四个 `verify_*.py` 把输出写死在**仓库根目录的 `logs/`**，那是运行产物目录，已在 `.gitignore` 中以 `/logs/` 忽略。
本次归档的原始日志是这些记录的**副本**，放在 [`docs/logs/2026-10-07/`](../docs/logs/2026-10-07/)：

- 重跑脚本**只会写进根目录 `logs/`，不会写进 `docs/logs/`**。
- 要归档就手工复制过去，并先确认没有覆盖已有证据（同名直接覆盖，不会提示）。
- `<仓库根>/logs/` 里的文件名与归档中的文件名一致，便于逐份对照。

## 3. 固件版本与脚本配对

| 固件目录 | 上电自报 | 可用脚本 | 对应归档日志 |
| --- | --- | --- | --- |
| `MecanumPS2Diagnostic` | `BOOT MecanumPS2 v1.1+diag`（**没有** `INFO` 命令） | `verify_pulse_vm_off.py` / `pulse_once.py --wheel FL\|RL\|FR\|RR` | [`诊断程序-VM关闭验证-2026-10-07.txt`](../docs/logs/2026-10-07/诊断程序-VM关闭验证-2026-10-07.txt) |
| `MecanumPS2WheelDiagnostic` | `BOOT MecanumPS2 v1.1+diag-wheels`（**没有** `INFO` 命令） | `verify_wheel_pulse_vm_off.py` | [`四通道诊断程序-VM关闭验证-2026-10-07.txt`](../docs/logs/2026-10-07/四通道诊断程序-VM关闭验证-2026-10-07.txt) |
| `MecanumPS2FourWheelDiagnostic` | `INFO firmware=MecanumPS2-v1.1+diag-all polarity=1,1,-1,-1 allCap=192 allMaxMs=300` | `verify_all_pulse_vm_off.py` / `pulse_once.py --wheel ALL` | [`四轮联动诊断-VM关闭验证-2026-10-07.txt`](../docs/logs/2026-10-07/四轮联动诊断-VM关闭验证-2026-10-07.txt) |
| `MecanumPS2MotionDiagnostic` | `INFO firmware=MecanumPS2-v1.1+diag-motion ...` + `MOTIONINFO maxDuty=192 maxMs=10000` | `verify_motion_vm_off.py` / `motion_sequence.py` / `pulse_once.py`（单轮） | [`麦轮运动组合诊断-VM关闭验证-2026-10-07.txt`](../docs/logs/2026-10-07/麦轮运动组合诊断-VM关闭验证-2026-10-07.txt) |

`MecanumPS2Diagnostic` 的 `-pulse` 是本归档为区分阶段加的标注，固件自己只报 `v1.1+diag`。

`verify_all_pulse_vm_off.py`、`verify_motion_vm_off.py`、`motion_sequence.py` 会**逐字校验** `INFO` 里的固件名与
极性 `1,1,-1,-1`。这是刻意的：拿 `diag-motion` 的脚本去连旧固件会读不到 `INFO` 直接失败，
避免把没校准的旧极性当成装车前进方向。

## 4. 编译与烧录

四个 sketch 都是 **Arduino ESP32 core 3.3.12、经典 ESP32（`esp32:esp32:esp32`）**，没有额外第三方库依赖。

```powershell
# 只编译（在仓库根目录执行）
arduino-cli compile --fqbn esp32:esp32:esp32 --build-path build/MecanumPS2MotionDiagnostic firmware/chassis/MecanumPS2MotionDiagnostic

# 编译并烧录
arduino-cli upload -p COM9 --fqbn esp32:esp32:esp32 firmware/chassis/MecanumPS2MotionDiagnostic
```

- 本机那套便携版 Arduino 的数据目录不在默认位置，**必须补 `--config-file <...>\arduino-cli.yaml`**，
  否则找不到 esp32 核心（原因见根 README §8）。
- 仓库路径含中文；工具链若在中文构建路径上报错，就用 `--build-path` 指到一个英文临时目录。
- `tools/build-chassis.ps1` 的 `-Sketch` 只接受 `MotorLinkCheck` / `MotorDriver` / `All`（`ValidateSet`），
  **不覆盖这四个诊断固件**，请直接用上面的 `arduino-cli` 命令。
- 编译产物留在已被忽略的 `build/` 下，**不提交**镜像、`.elf`、`.map` 等。

## 5. 各脚本做什么

### `verify_pulse_vm_off.py`（无参数）

旧版单轮诊断（`v1.1+diag`）的解析器与限时检查：先发若干非法 `PULSE`
（未知轮标签、duty 越界、时长越界、多余参数）确认都被 `ERR PULSE/interlock; LOCKED` 拒绝，
再发合法 `PULSE` 确认自报占空比与 `JOG DONE/STOP; LOCKED` 自动到期。

```powershell
python host\verify_pulse_vm_off.py
```

### `verify_wheel_pulse_vm_off.py`（无参数）

四通道诊断（`v1.1+diag-wheels`）：四个轮标签逐个 `PULSE`，检查**只有该轮非零、其余三轮为零**，
再检查自动到期、重叠命令拒绝与 `STOP`。

```powershell
python host\verify_wheel_pulse_vm_off.py
```

### `verify_all_pulse_vm_off.py`（无参数）

四轮联动诊断（`v1.1+diag-all`）：`ALLPULSE` 的两个方向、`PULSE` 四个轮、
非法参数、重叠命令、`STOP` 冷却、`PS2` 遥控模式互锁，共约 40 步。

```powershell
python host\verify_all_pulse_vm_off.py
```

### `verify_motion_vm_off.py`（无参数）

当前版（`v1.1+diag-motion`）最完整的一轮：十种 `MOVEPULSE` 组合逐个走一遍，
外加校准态、限值、自动到期、重叠、`STOP` 冷却与遥控互锁。
它用 `motion_sequence.MOVES` 计算期望占空比，**和 `motion_sequence.py` 共用同一份符号表**。

```powershell
python host\verify_motion_vm_off.py
```

### `pulse_once.py`（单次点动）

只发一次点动，不做整套验证。单轮 500ms 上限、`ALL` 300ms 上限。
选轮时 `--duty` 允许 ±1..255，`ALL` 时限制在 ±192。

```powershell
python host\pulse_once.py --wheel RL --duty -128 --log logs\RL反向单次-今天.txt
python host\pulse_once.py --wheel ALL --duty 192 --log logs\四轮正向单次-今天.txt
python host\pulse_once.py --wheel FL --duty 128 --log logs\FL正向单次-今天.txt --prepare 10
```

| 参数 | 含义 |
| --- | --- |
| `--wheel` | `FL` / `RL` / `FR` / `RR` / `ALL`（必填） |
| `--duty` | 非零带符号占空比；单轮 ±1..255，`ALL` ±192（必填） |
| `--log` | 记录文件路径（必填，父目录会自动创建） |
| `--prepare` | 发命令前等待人工确认的秒数，0..30，默认 5 |

### `motion_sequence.py`（十种组合，逐个带间隔）

按 `--gap` 间隔依次执行若干个命名组合，每个组合之间插入静止核查；任何 `ERR`、提前停止或
手柄按键都会**中止整段序列，且不重试**。

```powershell
# 只跑前进，10 秒，75.3% 占空比（架空观察用）
python host\motion_sequence.py --moves FWD --duration 10000 --duty 192 --log logs\麦轮前进10秒.txt

# 一次跑多个组合（每个组合只能出现一次）
python host\motion_sequence.py --moves FWD BACK LEFT RIGHT --duration 2000 --duty 128 --gap 8 --log logs\四向2秒.txt
```

| 参数 | 含义 |
| --- | --- |
| `--moves` | 一个或多个：`FWD` `BACK` `LEFT` `RIGHT` `FL` `FR` `BL` `BR` `CCW` `CW`（必填，同一组合不能重复） |
| `--duration` | 每个组合时长 50..10000 ms，默认 10000 |
| `--duty` | 1..192，默认 192 |
| `--prepare` | 首个组合前等待人工确认的秒数，0..30，默认 5 |
| `--gap` | 组合之间的静止间隔，5..30 秒，默认 5 |
| `--log` | 记录文件路径（必填） |

注意 `MOVEPULSE` 的 `FL`/`FR` 指**左前 / 右前斜行组合**，而单轮 `PULSE` 的 `FL`/`FR` 指轮标签，两者不是一回事。
组合符号按校准后的车辆逻辑方向换算，串口回显的是实际 GPIO 方向符号（前进 192 回显 `192,192,-192,-192`）。

## 6. 复现顺序

**第一步（VM 关闭，纯软件）** —— 这一步不需要动力，也不需要拆线：

1. 烧入 `MecanumPS2MotionDiagnostic`，确认上电 banner 打印 `LOCKED`、`STBY=0`。
2. **实体断开驱动功率**（只留 USB 逻辑供电）。
3. 依次跑 `verify_motion_vm_off.py`、`motion_sequence.py --moves FWD --duration 10000`。
   归档里对应 [`麦轮运动组合诊断-VM关闭验证`](../docs/logs/2026-10-07/麦轮运动组合诊断-VM关闭验证-2026-10-07.txt)
   与 [`麦轮组合脚本-完整10秒VM关闭验证`](../docs/logs/2026-10-07/麦轮组合脚本-完整10秒VM关闭验证-2026-10-07.txt)。

**第二步（带动力，架空）** —— 只有在第一步全绿、且换过测点后关闭动力、表笔稳定时再进：

1. 四轮**架空**，VM 设定 6V，手柄按键全部松开。
2. 先做**单轮**短测（`pulse_once.py`），确认方向与极性表一致。
3. 再做 `ALLPULSE` / `MOVEPULSE` 短测（300ms 起）。
4. 每次动作前后都用 `STATUS` 确认回到 `control=LOCKED STBY=0 duty=0,0,0,0`。

**不要**把这几步合成一次连续长测。归档里四轮持续联动的失败正是在超过单次短测时长的工况下出现的，
先做待测的输出电压诊断（见归档 §下一步待测），再考虑加长时长。

## 7. 安全与证据边界

- **软件停止不能替代断开驱动功率电源。** `STOP` 只把 `STBY` 拉低并清零占空比；
  要动线、换测点、插拔接头，先实体断电。
- 所有限时都是**软件超时**，靠主循环计时，只有 1 秒任务看门狗兜底，
  不是独立硬件电流、温度或时限保证。固件里 `CAP=76` / `ALL_PULSE_CAP=192` 都是**占空比上限，不是电流限制**。
- 这些脚本的 `PASS` 只覆盖：命令解析、自报占空比、`STBY`/`control` 状态字、自动到期与互锁。
  **不覆盖**：实际电流、VM 跌落、转速、轮子是否真的转、温升、以及快速瞬态。
- 插拔串口前先 `STOP`；脚本在 `finally` 里会补发 `STOP` 并再确认一次空闲。
- 归档中"只看见某一轮转"这类结论来自操作员现场观察，不在这些日志里，不能由日志反推。
