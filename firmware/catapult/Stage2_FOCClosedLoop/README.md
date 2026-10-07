# Stage2_FOCClosedLoop：第二阶段（低电压闭环空载测试）

> **导航**：[仓库总入口](../../../README.md) · [ESP32-2804 接线与烧录指南](../../../docs/ESP32-2804-FOC-guide.md) · [投掷机构-设计与安全](../../../docs/投掷机构-设计与安全.md) · [主固件 PS2Catapult](../PS2Catapult/README.md) · [Stage1](../Stage1_AS5600Test/README.md)

第二阶段：SimpleFOC 闭环空载测试。源码里 `CONFIG_CONFIRMED = false`，所以
**现在烧进去是安全锁定状态**：只会打印 `LOCKED`，电机不会动，也不会校准。

## 解除配置锁之前，逐条核对

- [ ] 接线逐根核对过（`IN1=25 IN2=26 IN3=27 EN=14`、`SDA=21 SCL=22`，共地）
- [ ] **驱动板 EN 的有效极性已确认**（程序假定高电平有效）
- [ ] 用万用表实测了驱动板输入电压，并把 `SUPPLY_VOLTAGE` 改成实测值
- [ ] 电机 U/V/W 已接好，电机牢固固定
- [ ] 编码器磁铁随转子转动且中心对准芯片（第一阶段 `MD=1`）
- [ ] **首次测试不装投掷杆**
- [ ] 把 `CONFIG_CONFIRMED` 改成 `true`

## 串口命令（115200）

| 键 | 作用 |
|---|---|
| `c` | 对齐 / 校准 —— **电机会动**（本版仅在 `!enabledState()` 时接受） |
| `+` | 目标 +1 rad/s 试转，最多 5 秒 |
| `-` | 目标 −1 rad/s 反向试转，最多 5 秒 |
| `0` | 请求零速度 |
| `x` | 立即软件禁用 |

## 安全

- 本程序**会让电机转起来**，`VOLTAGE_LIMIT` 只给 1.0V；**不要通过加大它来解决"电机不转"**。
- **软件停止不能替代断开驱动功率电源。** 校准时串口响应会变慢；要急停就拔驱动功率。
- 编码器供电有"接 5V"与"接 3.3V 但要去掉 R3"两说冲突；ESP32 的 SDA/SCL 是 3.3V 电平，
  **5V 上拉需要电平转换**，别直连。

## 构建 / 烧录

```powershell
.\tools\flash-catapult.ps1 -Stage 2              # 编译并烧录
.\tools\flash-catapult.ps1 -Stage 2 -CompileOnly # 只编译
```

依赖 `Simple FOC 2.4.0` 与 `esp32:esp32` 3.3.12。详细依据见 guide 第 5 节。
