# ESP32 + 2804 无刷电机：接线信息与烧录测试指南

整理日期：2026-10-05。依据当前对话整理；原始说明图片未在本次重新检视。文中的实物型号与参数采用此前对话记录，存在疑问时以你的实物及厂家资料为准。

## 1. 目前知道什么

| 项目 | 当前信息 | 确认程度 |
|---|---|---|
| 主控 | ESP32，此前识别为 ESP32-WROOM-32E | 需核对模块丝印；不是直接照厂家示例选 ESP32-S3 |
| 电机 | 2804 三相无刷电机，用于投掷机构项目 | 用户提供型号；“直流”在这里不代表两线有刷电机 |
| 电机参数 | 此前说明图解读为额定 12V、7 对磁极 | 本次未重看图片；代码先按 7 对，核对后使用 |
| 驱动板 | FOC 三相驱动板，控制口 IN1/IN2/IN3/EN/GND/FLT，输出 U/V/W | 此前照片解读；芯片疑似 DRV8313，尚不能写成确定型号 |
| 驱动电源范围 | 此前记录板上标注 8.2～24V | 不代表电机也能承受整个范围 |
| 编码器 | AS5600，I²C 接口 | 用户确认 |
| 电池 | 标称 12V、无型号锂离子电池 | 满电电压、串数、保护与电流能力未知 |
| 降压模块 | 提供 5V、3.3V、可调输出及 GND | 此前图片解读 |
| 已接线 | 电机—驱动板、驱动板—降压模块、驱动板—ESP32、编码器—ESP32、ESP32—降压模块 | 用户表示均已接好 |
| 测量 | 用户表示测得电压均符合预期 | 没有具体数值记录 |
| 软件与测试 | 曾计划使用本机 Arduino、ESP32 支持包、SimpleFOC | 尚无实际编译、上传或电机成功运行证据 |

“FOC 驱动板”主要提供功率级，不能由名字推断板上已经运行 FOC 算法。本方案由 ESP32 运行 SimpleFOC，AS5600 提供转子角度，驱动板把三相 PWM 转换为电机功率输出。

## 2. 烧录前填好的接线与参数表

以下 GPIO 是此前建议，用户尚未明确逐项确认。沿每根线核对，不要仅按线色判断。代码中的数字是 GPIO 编号，不是排针的第几个位置。

| 信号 | 此前建议 GPIO | 你的实际 GPIO |
|---|---:|---|
| IN1 | 25 | ______ |
| IN2 | 26 | ______ |
| IN3 | 27 | ______ |
| EN | 14 | ______ |
| SDA | 21 | ______ |
| SCL | 22 | ______ |
| 驱动 GND、编码器 GND | ESP32 GND | 共地已核对：______ |
| FLT | 暂不连接 | ______ |

请记录：驱动板输入端实测 ______ V；编码器 VCC ______ V；SDA 空闲 ______ V；SCL 空闲 ______ V；极对数 ______。

- 驱动板功率输入必须符合板子的要求。不能把 5V 或 3.3V 逻辑电源当成此前标注 8.2～24V 的驱动功率输入。
- 编码器曾出现“实物丝印接 5V”和“厂家图接 3.3V、5V 时去掉 R3”的冲突。按实际模块电路核对，不能仅凭 AS5600 芯片名称决定接法。ESP32 的 SDA/SCL 侧信号应为 3.3V 电平；5V 上拉需要适当的 I²C 双向电平转换或按厂家资料修改模块。
- 下载时建议断开降压模块到 ESP32 的 5V 供电线，USB 给 ESP32 供电，共地保留；核对编码器仍有电。首次不要同时接外部 5V 和 USB，除非已确认开发板的供电隔离设计。
- 编码器板保持固定，轴端磁铁随转子转动，磁铁中心对准芯片。电机牢固安装，首次测试不装投掷杆。
- 初次上传时断开驱动板功率电源。断电后才调整接线；电机电流走合适的电源线，不走杜邦线。

## 3. Arduino 环境与上传

1. 安装 Arduino IDE；开发板管理器安装 Espressif 的 `esp32` 支持包。
2. 库管理器安装 `Simple FOC`。记录 IDE、ESP32 支持包、SimpleFOC 的版本；本文示例不代表所有版本组合都已验证兼容。
3. USB 连接 ESP32，选择与实物匹配的开发板；常见 WROOM 开发板可选 `ESP32 Dev Module`，但最终以板卡资料为准。
4. 选择插拔 USB 后出现的串口。若没有串口，检查数据线和板载 USB 转串口芯片对应驱动。
5. 新建草图，粘贴下面一个测试程序，修改引脚，先点击“验证”，再点击“上传”。两个程序分别上传，不要合并两个 `setup()`/`loop()`。
6. 串口监视器波特率选 `115200`。若连接卡在下载阶段，可按板卡操作要求使用 BOOT/EN；不要把所有上传失败都当成程序问题。

## 4. 第一阶段：只测试 AS5600

驱动板功率电源保持断开。这个程序不需要 SimpleFOC，直接读取 AS5600 的状态和 12 位原始角度，并把 EN 拉低。这里假设驱动板 EN 高电平有效，必须与实物资料核对。

```cpp
#include <Wire.h>

constexpr int SDA_PIN = 21;  // 改为实际 GPIO
constexpr int SCL_PIN = 22;
constexpr int EN_PIN = 14;
constexpr uint8_t AS5600_ADDR = 0x36;

bool readRegs(uint8_t reg, uint8_t *out, uint8_t count) {
  Wire.beginTransmission(AS5600_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(AS5600_ADDR, count) != count) return false;
  for (uint8_t i = 0; i < count; ++i) out[i] = Wire.read();
  return true;
}

void setup() {
  pinMode(EN_PIN, OUTPUT);
  digitalWrite(EN_PIN, LOW);
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(20);
  Serial.println("AS5600 test: turn rotor by hand; driver power OFF");
}

void loop() {
  uint8_t status, angle[2];
  if (!readRegs(0x0B, &status, 1) || !readRegs(0x0C, angle, 2)) {
    Serial.println("I2C ERROR: check power, SDA/SCL, GND and address 0x36");
  } else {
    uint16_t raw = ((uint16_t(angle[0]) << 8) | angle[1]) & 0x0FFF;
    Serial.print("raw="); Serial.print(raw);
    Serial.print(" deg="); Serial.print(raw * 360.0f / 4096.0f, 2);
    Serial.print(" MD="); Serial.print((status & 0x20) != 0);
    Serial.print(" ML="); Serial.print((status & 0x10) != 0);
    Serial.print(" MH="); Serial.println((status & 0x08) != 0);
  }
  delay(100);
}
```

合格表现：手转一圈时原始角度覆盖约 0～4095，跨零点回绕正常；转子不动时读数基本稳定。`MD=1` 表示检测到磁铁；`ML=1` 表示磁场偏弱，`MH=1` 表示偏强。先处理通信和磁铁位置问题，再进入第二阶段。单圈原始角度回绕与 SimpleFOC 累计多圈角度是两种不同输出。

## 5. 第二阶段：低电压闭环空载测试

默认引脚仍是待核对的建议值。把 `SUPPLY_VOLTAGE` 改为驱动板输入端的实测值，把 `CONFIG_CONFIRMED` 改为 `true` 后才允许校准。

这个程序上电不会自动校准。输入 `c` 才执行对齐；校准时电机会运动。校准后输入 `+` 或 `-`，以目标 ±1 rad/s（约 ±9.5 rpm）短时试转；`x` 立即软件禁用，`0` 请求零速度。每次试转最多 5 秒，需要再次输入才继续。软件停止不能替代断开驱动电源。

```cpp
#include <SimpleFOC.h>
#include <Wire.h>

constexpr int IN1_PIN = 25, IN2_PIN = 26, IN3_PIN = 27;
constexpr int EN_PIN = 14, SDA_PIN = 21, SCL_PIN = 22;
constexpr int POLE_PAIRS = 7;
constexpr float SUPPLY_VOLTAGE = 12.0f; // 必须改为实测值
constexpr bool CONFIG_CONFIRMED = false; // 核对接线、EN极性、供电后改 true

MagneticSensorI2C sensor = MagneticSensorI2C(AS5600_I2C);
BLDCDriver3PWM driver = BLDCDriver3PWM(IN1_PIN, IN2_PIN, IN3_PIN, EN_PIN);
BLDCMotor motor = BLDCMotor(POLE_PAIRS);
bool prepared = false, aligned = false, running = false;
float target = 0;
unsigned long runStarted = 0, lastPrint = 0;

void stopMotor() {
  target = 0;
  motor.disable();
  running = false;
}

void setup() {
  pinMode(EN_PIN, OUTPUT);
  digitalWrite(EN_PIN, LOW);
  Serial.begin(115200);
  delay(1000);
  if (!CONFIG_CONFIRMED) {
    Serial.println("LOCKED: verify wiring and measured supply; edit CONFIG_CONFIRMED");
    return;
  }
  SimpleFOCDebug::enable(&Serial);
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(20);
  Wire.beginTransmission(0x36);
  if (Wire.endTransmission() != 0) {
    Serial.println("AS5600 not found; driver remains disabled");
    return;
  }
  sensor.init(&Wire);
  motor.linkSensor(&sensor);
  driver.voltage_power_supply = SUPPLY_VOLTAGE;
  driver.voltage_limit = SUPPLY_VOLTAGE;
  driver.pwm_frequency = 20000;
  if (!driver.init()) {
    Serial.println("Driver init failed");
    return;
  }
  motor.linkDriver(&driver);
  motor.torque_controller = TorqueControlType::voltage;
  motor.controller = MotionControlType::velocity;
  motor.voltage_limit = 1.0f;
  motor.voltage_sensor_align = 1.0f;
  motor.velocity_limit = 2.0f;
  motor.PID_velocity.P = 0.2f;
  motor.PID_velocity.I = 1.0f;
  motor.PID_velocity.D = 0.0f;
  motor.LPF_velocity.Tf = 0.05f;
  motor.useMonitoring(Serial);
  motor.init();
  motor.disable();
  prepared = true;
  Serial.println("Ready: power driver, then c=align, +/-=test, 0=zero speed, x=disable");
}

void loop() {
  if (!prepared) { delay(10); return; }
  while (Serial.available()) {
    char cmd = Serial.read();
    if (cmd == 'x') {
      stopMotor();
      Serial.println("Disabled");
    } else if (cmd == 'c' && !aligned) {
      Serial.println("Aligning: rotor will move");
      motor.enable();
      aligned = motor.initFOC();
      stopMotor();
      Serial.println(aligned ? "Alignment OK; send + or -" : "Alignment FAILED; inspect hardware");
    } else if (aligned && (cmd == '+' || cmd == '-' || cmd == '0')) {
      target = cmd == '+' ? 1.0f : (cmd == '-' ? -1.0f : 0.0f);
      motor.enable();
      running = true;
      runStarted = millis();
    }
  }
  if (running) {
    if (millis() - runStarted >= 5000) {
      stopMotor();
      Serial.println("5-second test finished; disabled");
    } else {
      motor.loopFOC();
      motor.move(target);
      if (millis() - lastPrint >= 200) {
        lastPrint = millis();
        Serial.print("target="); Serial.print(target);
        Serial.print(" velocity="); Serial.println(motor.shaft_velocity);
      }
    }
  } else {
    sensor.update();
  }
}
```

`driver.voltage_limit` 在这里保持实际母线电压，`motor.voltage_limit` 限制控制器请求的电机电压。1V 是测试起点，不保证所有电机都能完成对齐，也不是硬件电流限制。未配置电流采样，这个程序属于电压模式 FOC，不能证明输出电流安全或验证额定负载能力。

速度 PID 参数同样只是起点。持续抖动、不转却发热、校准失败时先断电检查，不要直接提高电压。校准调用期间串口停止命令不能及时处理；需要停止时断开驱动功率电源。程序仅做启动前的地址检查，没有完整的运行时编码器失联保护。

## 6. 执行顺序与判断结果

1. 驱动功率断电，上传第一阶段程序，手动确认角度与磁场状态。
2. 核对引脚、极对数、驱动板 EN 有效极性，填写实测供电；上传第二阶段程序。
3. 打开串口监视器，确认出现 `Ready`；给驱动板接通合格电源，再发送 `c`。
4. 若校准成功，发送 `+` 试转 5 秒，观察方向、平稳程度、实际速度与温升；之后发送 `-` 测试反向。
5. 连续、平稳地双向低速运行，且没有明显异常发热，才说明基本通信与驱动链路通过空载测试。记录串口日志与版本。
6. 投掷杆、减速机构、限位、负载和投掷动作是后续独立工作；本次空载通过不等于适合直接带投掷杆工作。

## 7. 常见问题

| 现象 | 优先检查 |
|---|---|
| 没有串口 | USB 数据线、串口驱动、板卡连接 |
| 编译失败 | 保存完整首个错误，核对 SimpleFOC 与 ESP32 支持包版本；不要盲目改接线 |
| AS5600 无响应 | 编码器供电、SDA/SCL、共地、上拉与电平 |
| 角度不变或乱跳 | 磁铁是否随转子转动、磁铁位置、固定方式、磁场状态 |
| PWM 初始化失败 | 实际 GPIO 是否可输出、板型、库与支持包兼容性 |
| 校准失败或抖动 | 编码器角度、极对数、相线、驱动供电与 EN；确认驱动板确实兼容 3PWM |
| ESP32 重启 | 供电跌落、共地与电流回路、接触问题、电机干扰 |
| 转向与预期相反 | 先用目标速度正负号定义方向；相线改变后重新上电校准 |

## 8. 官方参考与交接信息

- [SimpleFOC：3PWM 驱动](https://docs.simplefoc.com/bldcdriver3pwm)
- [SimpleFOC：I²C 磁编码器](https://docs.simplefoc.com/magnetic_sensor_i2c)
- [SimpleFOC：电压模式](https://docs.simplefoc.com/voltage_torque_mode)
- [AS5600 厂家产品页与数据手册入口](https://ams-osram.com/products/sensors/position-sensors/ams-as5600-position-sensor)

继续寻求帮助时一起提供：实物引脚表、驱动电压、编码器供电与信号电平、板卡和芯片丝印、三个软件版本、完整编译错误或校准串口日志。此前提出的远程电脑插件连接尚无成功证据；本文没有宣称已经在你的本机安装、编译或烧录。
