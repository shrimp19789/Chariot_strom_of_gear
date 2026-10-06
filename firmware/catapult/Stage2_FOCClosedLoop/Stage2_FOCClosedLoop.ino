/*
 * ============================================================================
 *  Stage2_FOCClosedLoop  —  第二阶段：低电压闭环空载测试 (SimpleFOC)
 * ============================================================================
 *
 *  依据: docs\ESP32-2804-FOC-guide.md  第 5 节
 *  库  : Simple FOC 2.4.0 (本机已安装), ESP32 core 3.3.12
 *
 * ---------------------------------------------------------------------------
 *  ★★ 这个程序会让电机转起来, 上电前务必逐条确认 ★★
 * ---------------------------------------------------------------------------
 *  必须先做完 Stage1, 并且:
 *    [ ] 接线已逐根核对 (IN1=25 IN2=26 IN3=27 EN=14 SDA=21 SCL=22, 共地)
 *    [ ] 驱动板 EN 的有效极性已确认 (本程序假定【高电平有效】)
 *
 *        —— 关于 EN 极性: 驱动板照片 OCR 出芯片是 TI DRV8313, 按数据手册:
 *           板上 EN    = DRV8313 的 nSLEEP (总使能), 低=睡眠/输出高阻, 高=工作
 *           板上 IN1-3 = DRV8313 的 EN1/EN2/EN3 (三相 PWM 输入)
 *           板上 FLT   = DRV8313 的 nFAULT (开漏, 所以不接是对的)
 *           所以"高电平=使能"成立, SimpleFOC 默认的 enable_active_high 也匹配。
 *           把 EN 拉低 = 睡眠 = 输出高阻, 任何时候都是安全状态。
 *           (唯一例外: 若板上加了反相级, 极性会翻转 —— 所以首次务必先断开功率电源)
 *    [ ] 已用万用表实测驱动板输入电压, 并把 SUPPLY_VOLTAGE 改成实测值
 *    [ ] 电机三相线 U/V/W 已接好, 电机牢固固定
 *    [ ] 编码器磁铁随转子转动, 且磁铁中心对准芯片 (Stage1 的 MD=1)
 *    [ ] 首次测试【不要装投掷杆】
 *    [ ] 把下面 CONFIG_CONFIRMED 改成 true
 *
 * ---------------------------------------------------------------------------
 *  为什么 motor.voltage_limit 只用 1.0V —— 这不是随便定的
 * ---------------------------------------------------------------------------
 *  这颗 2804 电机: 相电阻 Rs ≈ 2.3Ω, 额定电流 0.5A, 最大 2A。
 *  堵转电流 ≈ 施加电压 / Rs:
 *        1.0V / 2.3Ω  ≈ 0.43A   <- 与额定 0.5A 相当, 安全
 *        3.0V / 2.3Ω  ≈ 1.3 A   <- 已超额定, 靠"1A 以内温升正常"硬扛
 *       12.0V / 2.3Ω  ≈ 5.2 A   <- 远超 2A 上限, 37g 的小电机撑不住
 *  本程序【没有电流采样】, 属于电压模式 FOC, 软件无法限制真实电流。
 *  所以【不要因为"不转"就直接加大这个值】, 先断电查接线/极对数/编码器。
 *
 * ---------------------------------------------------------------------------
 *  串口交互 (115200)
 * ---------------------------------------------------------------------------
 *   c  执行对齐/校准 (电机会动, 请确保周围没人没障碍)
 *   +  目标 +1 rad/s (约 +9.5 rpm) 试转, 最多 5 秒
 *   -  目标 -1 rad/s 反向试转, 最多 5 秒
 *   0  请求零速度
 *   x  立即软件禁用
 *
 *  注意: 软件停止不能替代断开驱动功率电源; 校准时串口命令响应会变慢。
 *
 * ============================================================================
 */

#include <SimpleFOC.h>
#include <Wire.h>

/* ---------------------------------------------------------------------------
 *  配置区  ——  逐项核对后再改 CONFIG_CONFIRMED
 * ---------------------------------------------------------------------------
 *  25/26/27/14 不是猜的: 板上此前烧录的 "2804 OPEN-LOOP MOTOR TEST" 固件
 *  自报 "wiring: IN1/2/3 -> 25/26/27 , EN -> 14"。
 */
constexpr int   IN1_PIN = 25;
constexpr int   IN2_PIN = 26;
constexpr int   IN3_PIN = 27;
constexpr int   EN_PIN  = 14;
constexpr int   SDA_PIN = 21;
constexpr int   SCL_PIN = 22;

/* 2804 是 12 槽 14 极 (12N14P) -> 极对数 = 14/2 = 7
 * 依据: 电机规格图 "槽极数 槽数12/极数14  12N14P" 与 "极对数 7对" */
constexpr int   POLE_PAIRS = 7;

/* !!! 必须改成驱动板输入端【实测】电压, 不要照抄 12.0 !!! */
constexpr float SUPPLY_VOLTAGE = 12.0f;

/* 测试用的电机电压上限 (见上面注释的堵转电流计算) */
constexpr float MOTOR_VOLTAGE_LIMIT = 1.0f;

/* 每次试转最长 5 秒 */
constexpr uint32_t MAX_RUN_MS = 5000;

/* 全部核对完改成 true 才允许校准 —— 这是防止误操作的最后一道闸 */
constexpr bool CONFIG_CONFIRMED = false;

/* ---------------------------------------------------------------------------
 *  SimpleFOC 对象
 * ------------------------------------------------------------------------- */
MagneticSensorI2C sensor = MagneticSensorI2C(AS5600_I2C);
BLDCDriver3PWM   driver = BLDCDriver3PWM(IN1_PIN, IN2_PIN, IN3_PIN, EN_PIN);
BLDCMotor        motor  = BLDCMotor(POLE_PAIRS);

bool          prepared = false;
bool          aligned  = false;
bool          running  = false;
float         target   = 0;
uint32_t      runStarted = 0;
uint32_t      lastPrint  = 0;

void stopMotor() {
  target = 0;
  motor.disable();
  running = false;
}

/* ---------------------------------------------------------------------------
 *  setup
 * ------------------------------------------------------------------------- */
void setup() {
  /* 先确保驱动关闭 */
  pinMode(EN_PIN, OUTPUT);
  digitalWrite(EN_PIN, LOW);

  Serial.begin(115200);
  delay(1000);

  Serial.println();
  Serial.println(F("============================================================"));
  Serial.println(F("  Stage 2: 2804 闭环 FOC 空载测试 (SimpleFOC)"));
  Serial.println(F("============================================================"));

  if (!CONFIG_CONFIRMED) {
    Serial.println(F("  [LOCKED] CONFIG_CONFIRMED 还是 false, 拒绝启动。"));
    Serial.println(F("  请先按源码顶部注释逐条核对, 然后把"));
    Serial.println(F("    constexpr bool CONFIG_CONFIRMED = false;"));
    Serial.println(F("  改成 true, 重新上传。"));
    Serial.println(F("  当前电机保持禁用状态。"));
    Serial.println(F("============================================================"));
    return;
  }

  Serial.println(F("  检查 AS5600 ..."));
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(20);
  Wire.beginTransmission(0x36);
  if (Wire.endTransmission() != 0) {
    Serial.println(F("  [FAIL] 0x36 无应答, 编码器不在线"));
    Serial.println(F("  驱动保持禁用, 不继续初始化。请先跑 Stage1。"));
    Serial.println(F("============================================================"));
    return;
  }
  Serial.println(F("  [OK]   AS5600 在线"));

  SimpleFOCDebug::enable(&Serial);

  sensor.init(&Wire);
  motor.linkSensor(&sensor);

  driver.voltage_power_supply = SUPPLY_VOLTAGE;
  driver.voltage_limit       = SUPPLY_VOLTAGE;   // 驱动器不额外限制, 由 motor.voltage_limit 限制
  driver.pwm_frequency       = 20000;
  if (!driver.init()) {
    Serial.println(F("  [FAIL] 驱动器初始化失败"));
    Serial.println(F("  检查 IN1/IN2/IN3/EN 是否为 ESP32 可输出 PWM 的引脚"));
    Serial.println(F("============================================================"));
    return;
  }
  Serial.println(F("  [OK]   3PWM 驱动器已初始化"));

  motor.linkDriver(&driver);
  motor.torque_controller  = TorqueControlType::voltage;   // 无电流采样 -> 电压模式
  motor.controller         = MotionControlType::velocity;
  motor.voltage_limit      = MOTOR_VOLTAGE_LIMIT;
  motor.voltage_sensor_align = MOTOR_VOLTAGE_LIMIT;
  motor.velocity_limit     = 2.0f;

  /* 速度环 PID 只是起点, 不是调好的参数 */
  motor.PID_velocity.P  = 0.2f;
  motor.PID_velocity.I  = 1.0f;
  motor.PID_velocity.D  = 0.0f;
  motor.LPF_velocity.Tf = 0.05f;

  motor.useMonitoring(Serial);
  motor.init();
  motor.disable();          // init() 可能会使能, 这里明确关掉
  prepared = true;

  Serial.println(F("------------------------------------------------------------"));
  Serial.print  (F("  极对数=")); Serial.print(POLE_PAIRS);
  Serial.print  (F("  母线电压=")); Serial.print(SUPPLY_VOLTAGE, 1);
  Serial.print  (F("V  电机电压上限=")); Serial.print(MOTOR_VOLTAGE_LIMIT, 1);
  Serial.println(F("V"));
  Serial.println(F("  Ready: 先给驱动板接通合格电源, 然后:"));
  Serial.println(F("    c = 对齐校准(电机会动)   + / - = 正/反试转 5 秒"));
  Serial.println(F("    0 = 零速   x = 立即禁用"));
  Serial.println(F("  当前电机处于禁用状态。"));
  Serial.println(F("============================================================"));
}

/* ---------------------------------------------------------------------------
 *  loop
 * ------------------------------------------------------------------------- */
void loop() {
  if (!prepared) { delay(10); return; }

  while (Serial.available()) {
    char cmd = Serial.read();

    if (cmd == 'x') {
      stopMotor();
      Serial.println(F("  -> 已禁用 (x)"));

    } else if (cmd == 'c') {
      if (aligned) {
        Serial.println(F("  -> 已经校准过了, 直接发 + 或 -"));
      } else {
        Serial.println(F("  -> 开始对齐, 电机会动!"));
        motor.enable();
        /* initFOC() 返回 1 = 成功, 0 = 失败 (与 POSIX 相反, 别写反) */
        aligned = (motor.initFOC() == 1);
        stopMotor();
        if (aligned) {
          Serial.println(F("  -> 对齐成功, 可以发 + 或 - 试转"));
        } else {
          Serial.println(F("  -> 对齐失败, 电机已禁用"));
          Serial.println(F("     依次检查: 编码器角度是否随转子变化 / 极对数 / 相线顺序"));
          Serial.println(F("               EN 极性 / 驱动供电 / 磁铁位置"));
        }
      }

    } else if (aligned && (cmd == '+' || cmd == '-' || cmd == '0')) {
      target = (cmd == '+') ? 1.0f : ((cmd == '-') ? -1.0f : 0.0f);
      motor.enable();
      running = true;
      runStarted = millis();
      Serial.print(F("  -> 目标 ")); Serial.print(target, 2); Serial.println(F(" rad/s"));
    }
  }

  if (running) {
    if (millis() - runStarted >= MAX_RUN_MS) {
      stopMotor();
      Serial.println(F("  -> 5 秒试转结束, 已禁用"));
    } else {
      motor.loopFOC();
      motor.move(target);
      if (millis() - lastPrint >= 200) {
        lastPrint = millis();
        Serial.print(F("  target=")); Serial.print(target, 2);
        Serial.print(F("  velocity=")); Serial.print(motor.shaft_velocity, 2);
        Serial.print(F("  Uq=")); Serial.println(motor.voltage.q, 2);
      }
    }
  } else {
    sensor.update();          // 待机时也刷新角度, 方便观察
  }
}
