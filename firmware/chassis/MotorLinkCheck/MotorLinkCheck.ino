/*
 * ============================================================================
 *  MotorLinkCheck  —  开发板 <-> 奇果派(QGPMaker) 电机驱动板  链路自检 / 诊断
 * ============================================================================
 *
 *  适用开发板: ESP32 全系 (esp32 core) 与 Arduino UNO/Nano (AVR core)
 *  同一份源码两种板子都能编译 —— 因为现在还无法 100% 确认你手上的板子是哪一款。
 *
 * ---------------------------------------------------------------------------
 *  这个固件做什么
 * ---------------------------------------------------------------------------
 *  1) 确认开发板能不能"看到"电机驱动板 —— I2C 总线扫描 + PCA9685 寄存器回读
 *  2) 区分几种失败原因                  —— 总线被拉死 / 地址对不上 / 芯片不响应
 *  3) 逐路点动测试 M1~M4                —— 确认电机真的转得起来
 *  4) 可选: 接一个 ACS712/采样电阻      —— 自动判断"电机是否真的接上了"
 *
 *  为什么要"回读"而不只是"扫描到地址"
 *  ------------------------------------
 *  I2C 扫描只能证明"某个地址上有东西应答"。总线干扰、地址冲突、别的 I2C 器件
 *  都可能造成误判。本固件会往 PCA9685 的 MODE1 寄存器写已知图案再读回来比对,
 *  只有写进去什么就读出什么, 才判定驱动板真的在线。
 *
 *  关于"电机是否接上"的重要说明
 *  ---------------------------
 *  这块驱动板(以及所有 PCA9685 + H桥 的驱动板)都没有电流检测电路,
 *  所以**软件无法直接感知电机线圈是否存在**。本固件给两条路:
 *    a) TEST: 让每路输出一个短促的 PWM, 人眼/耳朵确认电机是否转动   —— 默认
 *    b) SENSE_PIN: 外接 ACS712/采样电阻后, 自动判断电流是否达标     —— 可选
 *
 * ---------------------------------------------------------------------------
 *  串口命令 (115200 8N1)
 * ---------------------------------------------------------------------------
 *   CHECK                完整自检并打印报告      (开机自动执行一次)
 *   SCAN                 只做 I2C 全总线扫描
 *   TEST ALL             依次点动 M1~M4
 *   TEST <1-4>           点动指定电机
 *   M <1-4> <-255..255>  手动驱动某路电机 (正=正转, 负=反转, 0=停)
 *   STOP                 全部停止
 *   I2C <sda> <scl>      改用自定义 I2C 引脚并重新自检
 *   HELP                 帮助
 *
 * ---------------------------------------------------------------------------
 *  接线
 * ---------------------------------------------------------------------------
 *   ESP32 DevKit : SDA=GPIO21, SCL=GPIO22  (默认引脚, 不用改)
 *   Arduino UNO  : SDA=A4,     SCL=A5      (默认引脚, 不用改)
 *   驱动板       : 直接插在开发板上; 逻辑电由开发板供给, 所以 I2C 通不通与
 *                  电机电源无关
 *   电机电源 VM  : 6~12V 动力电池接驱动板 DC 口 —— I2C 通了不代表电机会转!
 *
 * ============================================================================
 */

#include <Wire.h>
#include <stdarg.h>

#define FW_NAME    "MotorLinkCheck"
#define FW_VERSION "1.1"

#define SERIAL_BAUD       115200
#define MOTOR_TEST_SPEED  200      // 点动测试的 PWM 值 (0-255)
#define MOTOR_TEST_MS     700      // 每路点动持续时间
#define I2C_TIMEOUT_MS    20       // 单次 I2C 事务超时 (仅 ESP32 支持设置)

/* ---- 可选: 电流采样。接好 ACS712/采样电阻后把 SENSE_PIN 改成实际引脚 ----
 *   ACS712-05B 灵敏度 185mV/A, ACS712-20A 为 100mV/A, ACS712-30A 为 66mV/A
 *   零电流时输出约为 VCC/2 (5V 供电 -> 2500mV)
 *   留 -1 表示不启用, 此时点动测试需要人工确认电机是否转动
 */
#ifndef SENSE_PIN
#define SENSE_PIN        -1
#endif
#ifndef SENSE_MV_PER_A
#define SENSE_MV_PER_A   185.0f
#endif
#ifndef SENSE_ZERO_MV
#define SENSE_ZERO_MV    2500.0f
#endif
#ifndef SENSE_ACTIVE_MA
#define SENSE_ACTIVE_MA  80.0f     // 判定"电机接上了"的最小电流 (mA)
#endif

/* ---------------------------------------------------------------------------
 *  跨平台小工具
 * ------------------------------------------------------------------------- */

/* ESP32 的 HardwareSerial 有 printf, AVR 没有, 所以统一走这个函数 */
static void pf(const char *fmt, ...) {
  char buf[220];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print(buf);
}

/* 板型名 */
#if defined(ESP32)
  #define BOARD_NAME "ESP32"
#elif defined(__AVR_ATmega328P__) || defined(__AVR_ATmega168__)
  #define BOARD_NAME "Arduino UNO/Nano (AVR)"
#elif defined(__AVR_ATmega2560__)
  #define BOARD_NAME "Arduino Mega2560 (AVR)"
#else
  #define BOARD_NAME "Unknown"
#endif

/* 默认 I2C 引脚提示 */
#if defined(ESP32)
  #define DEFAULT_I2C_HINT "SDA=21 SCL=22"
#elif defined(__AVR__)
  #define DEFAULT_I2C_HINT "SDA=A4 SCL=A5"
#else
  #define DEFAULT_I2C_HINT "board default"
#endif

/* 读电压(mV), 两种平台各自的正确做法 */
#if SENSE_PIN >= 0
static uint32_t readMilliVolts() {
#if defined(ESP32)
  return analogReadMilliVolts(SENSE_PIN);
#else
  /* AVR: 10 位 ADC, 默认 5V 参考 */
  return (uint32_t)((uint32_t)analogRead(SENSE_PIN) * 5000UL / 1023UL);
#endif
}
#endif

/* ---------------------------------------------------------------------------
 *  PCA9685 寄存器
 * ------------------------------------------------------------------------- */
#define PCA_MODE1      0x00
#define PCA_PRESCALE   0xFE
#define PCA_LED0_ON_L  0x06

/* 驱动板电机通道映射
 *
 *  重要: 这里用的是厂家库 QGPMaker_MotorShield.cpp 里 getMotor() 实际硬编码的通道号,
 *  而不是头文件顶部那几个 MOTOR1_A / MOTOR1_B 宏 —— 那些宏是 Adafruit 老版本的遗留,
 *  V5.2/V5.6 库的直流电机路径根本没用它们。以实际库为准, 才能保证本自检固件与
 *  MotorDriver 固件驱动的是同一组通道。
 *
 *   每路电机 = 一个 PWM 通道(IN1, 调速) + 一个电平通道(IN2, 定方向)
 */
struct MotorPort { uint8_t in1; uint8_t in2; };

static const MotorPort MOTOR_PORT[4] = {
  {  8,  9 },   // M1 : in1=8  in2=9
  { 10, 11 },   // M2 : in1=10 in2=11
  { 15, 14 },   // M3 : in1=15 in2=14
  { 13, 12 },   // M4 : in1=13 in2=12
};

/* ---------------------------------------------------------------------------
 *  全局状态
 * ------------------------------------------------------------------------- */
static int      g_sda = -1;              // -1 = 使用开发板默认 I2C 引脚
static int      g_scl = -1;
static bool     g_wireReady   = false;
static uint8_t  g_pcaAddr     = 0;       // 0 = 未找到
static bool     g_linkOk      = false;
static uint8_t  g_scanAddrs[16];
static uint8_t  g_scanCount   = 0;
static uint16_t g_nackCount   = 0;       // endTransmission()==2  总线正常但该地址无器件
static uint16_t g_timeoutCount = 0;      // endTransmission()==5  总线被拉死/无上拉
static char     g_msg[160]    = "not run yet";

/* ---------------------------------------------------------------------------
 *  I2C 底层
 * ------------------------------------------------------------------------- */
static bool i2cWrite8(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static uint8_t i2cRead8(uint8_t addr, uint8_t reg) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission() != 0) return 0xFF;
  if (Wire.requestFrom(addr, (uint8_t)1) != 1) return 0xFF;
  return (uint8_t)Wire.read();
}

/* 只探测地址是否存在, 顺便统计错误类型 */
static bool i2cPing(uint8_t addr) {
  Wire.beginTransmission(addr);
  uint8_t err = Wire.endTransmission();
  if (err == 2)      g_nackCount++;
  else if (err == 5) g_timeoutCount++;
  return err == 0;
}

static void i2cScan() {
  g_scanCount    = 0;
  g_nackCount    = 0;
  g_timeoutCount = 0;

  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    if (i2cPing(addr)) {
      if (g_scanCount < sizeof(g_scanAddrs)) g_scanAddrs[g_scanCount] = addr;
      g_scanCount++;
    }
    yield();
  }
}

/* ---------------------------------------------------------------------------
 *  PCA9685 操作
 * ------------------------------------------------------------------------- */

/* 写图案 + 回读比对, 确认这颗芯片真的是 PCA9685 而且在正常工作 */
static bool pcaProbe(uint8_t addr) {
  /* 图案 1: 打开自动递增 AI(bit5) */
  if (!i2cWrite8(addr, PCA_MODE1, 0x20)) return false;
  uint8_t rb = i2cRead8(addr, PCA_MODE1);
  if ((rb & 0x20) == 0) return false;          // AI 位写不进去 -> 不是 PCA9685

  /* 图案 2: 置 SLEEP(bit4) */
  if (!i2cWrite8(addr, PCA_MODE1, 0x10)) return false;
  rb = i2cRead8(addr, PCA_MODE1);
  if ((rb & 0x10) == 0) return false;          // SLEEP 位写不进去 -> 不是 PCA9685

  /* 图案 3: 清 SLEEP, 恢复自动递增, 让芯片回到可用状态 */
  i2cWrite8(addr, PCA_MODE1, 0x20);
  delay(1);
  return true;
}

static void pcaSetFreq(uint8_t addr, uint16_t freq) {
  if (freq < 1) freq = 1;
  /* prescale = round(25MHz / (4096 * freq * 0.9)) - 1, 乘 0.9 补偿内部振荡器过冲 */
  uint32_t denom = (uint32_t)4096 * (uint32_t)freq * 9UL / 10UL;
  uint32_t prescale = (25000000UL + denom / 2) / denom;
  if (prescale > 0) prescale -= 1;
  if (prescale > 255) prescale = 255;

  uint8_t oldmode = i2cRead8(addr, PCA_MODE1);
  uint8_t newmode = (oldmode & 0x7F) | 0x10;      // SLEEP=1
  i2cWrite8(addr, PCA_MODE1, newmode);
  i2cWrite8(addr, PCA_PRESCALE, (uint8_t)prescale);
  i2cWrite8(addr, PCA_MODE1, oldmode);
  delay(5);
  i2cWrite8(addr, PCA_MODE1, oldmode | 0x80 | 0x20);  // RESTART + AI
}

/* 全 16 通道清零 = 所有电机停 */
static void pcaAllOff(uint8_t addr) {
  for (uint8_t ch = 0; ch < 16; ch++) {
    i2cWrite8(addr, PCA_LED0_ON_L + 4 * ch,     0);
    i2cWrite8(addr, PCA_LED0_ON_L + 4 * ch + 1, 0);
    i2cWrite8(addr, PCA_LED0_ON_L + 4 * ch + 2, 0);
    i2cWrite8(addr, PCA_LED0_ON_L + 4 * ch + 3, 0);
  }
}

static void pcaSetPWM(uint8_t ch, uint16_t on, uint16_t off) {
  if (g_pcaAddr == 0) return;
  i2cWrite8(g_pcaAddr, PCA_LED0_ON_L + 4 * ch,     (uint8_t)(on & 0xFF));
  i2cWrite8(g_pcaAddr, PCA_LED0_ON_L + 4 * ch + 1, (uint8_t)(on >> 8));
  i2cWrite8(g_pcaAddr, PCA_LED0_ON_L + 4 * ch + 2, (uint8_t)(off & 0xFF));
  i2cWrite8(g_pcaAddr, PCA_LED0_ON_L + 4 * ch + 3, (uint8_t)(off >> 8));
}

/* 等价于厂家库 setPin(pin, val): true=该通道常亮(4096), false=灭(0) */
static void pcaSetPin(uint8_t ch, bool val) {
  if (val) pcaSetPWM(ch, 4096, 0);
  else     pcaSetPWM(ch, 0, 0);
}

/* ---------------------------------------------------------------------------
 *  电机驱动 (复刻厂家 QGPMaker_DCMotor 的逻辑)
 *    speed > 0 : 正转 (IN2 拉低, IN1 输出 PWM)
 *    speed < 0 : 反转 (IN1 拉低, IN2 输出 PWM)
 *    speed = 0 : 释放 (两路都拉低)
 * ------------------------------------------------------------------------- */
static void motorRun(uint8_t motor, int speed) {
  if (motor < 1 || motor > 4) return;
  const MotorPort &p = MOTOR_PORT[motor - 1];

  if (speed > 255)  speed = 255;
  if (speed < -255) speed = -255;

  if (speed > 0) {
    pcaSetPin(p.in2, false);
    pcaSetPWM(p.in1, 0, (uint16_t)speed * 16);
  } else if (speed < 0) {
    pcaSetPin(p.in1, false);
    pcaSetPWM(p.in2, 0, (uint16_t)(-speed) * 16);
  } else {
    pcaSetPin(p.in1, false);
    pcaSetPin(p.in2, false);
  }
}

static void motorStopAll() {
  if (g_pcaAddr == 0) return;
  for (uint8_t m = 1; m <= 4; m++) motorRun(m, 0);
}

/* ---------------------------------------------------------------------------
 *  可选电流采样
 * ------------------------------------------------------------------------- */
#if SENSE_PIN >= 0
static int senseCurrentMa() {
  uint32_t acc = 0;
  for (uint8_t i = 0; i < 16; i++) { acc += readMilliVolts(); delayMicroseconds(200); }
  float mv = acc / 16.0f;
  return (int)((mv - SENSE_ZERO_MV) / SENSE_MV_PER_A * 1000.0f);
}
#endif

/* ---------------------------------------------------------------------------
 *  点动测试
 * ------------------------------------------------------------------------- */
static void testMotor(uint8_t m) {
  pf("#   M%u 正转 ... ", m);
  motorRun(m, 0);
  delay(50);
#if SENSE_PIN >= 0
  int zero = senseCurrentMa();
#endif
  for (int s = 0; s <= MOTOR_TEST_SPEED; s += 20) { motorRun(m, s); delay(12); }
  delay(MOTOR_TEST_MS);

#if SENSE_PIN >= 0
  {
    int mA = senseCurrentMa();
    pf("电流 %d mA -> %s\n", mA,
       (mA - zero) >= (int)SENSE_ACTIVE_MA ? "检测到电机" : "疑似未接电机");
  }
  motorRun(m, 0);
  delay(300);
#else
  motorRun(m, 0);
  Serial.println(F("已停止 (请确认电机是否转动)"));
  delay(300);
#endif
}

/* ---------------------------------------------------------------------------
 *  自检主流程
 * ------------------------------------------------------------------------- */
static void runCheck() {
  Serial.println();
  Serial.println(F("============================================================"));
  pf("  %s v%s   开发板: %s\n", FW_NAME, FW_VERSION, BOARD_NAME);
  Serial.println(F("============================================================"));

  /* 1. 启动 I2C */
  if (!g_wireReady) {
#if defined(ESP32)
    if (g_sda < 0) { Wire.begin();                                       Serial.println(F("[1/4] I2C  : 使用开发板默认引脚")); }
    else           { Wire.begin(g_sda, g_scl);                           pf("[1/4] I2C  : 使用自定义引脚 SDA=%d SCL=%d\n", g_sda, g_scl); }
    Wire.setTimeOut(I2C_TIMEOUT_MS);
#else
    /* AVR (UNO/Nano/Mega) 的 I2C 硬件引脚是固定的, TwoWire::begin() 不接受引脚参数 */
    if (g_sda < 0) {
      Wire.begin();
      Serial.println(F("[1/4] I2C  : 使用开发板默认引脚 (SDA=A4 SCL=A5)"));
    } else {
      Serial.println(F("[1/4] I2C  : AVR 的 I2C 引脚固定在 A4/A5, 无法更改, 忽略 I2C 命令"));
      g_sda = -1; g_scl = -1;
      Wire.begin();
    }
#endif
    g_wireReady = true;
    delay(50);
  }

  /* 2. 全总线扫描 */
  i2cScan();
  pf("[2/4] 扫描 : 发现 %u 个 I2C 器件", g_scanCount);
  if (g_scanCount) {
    Serial.print(F("  ->"));
    for (uint8_t i = 0; i < g_scanCount && i < sizeof(g_scanAddrs); i++)
      pf(" 0x%02X", g_scanAddrs[i]);
  }
  Serial.println();

  /* 3. 在扫描结果里找 PCA9685 (厂家默认 0x60, 也兼容 0x40~0x47 与 0x60~0x7F) */
  g_pcaAddr = 0;
  g_linkOk  = false;
  g_msg[0]  = 0;

  uint8_t candidates[16];
  uint8_t nCand = 0;
  for (uint8_t i = 0; i < g_scanCount && i < sizeof(g_scanAddrs); i++) {
    uint8_t a = g_scanAddrs[i];
    bool typical = (a >= 0x40 && a <= 0x47) || (a >= 0x60 && a <= 0x7F);
    if (typical && nCand < sizeof(candidates)) candidates[nCand++] = a;
  }

  for (uint8_t i = 0; i < nCand; i++) {
    if (pcaProbe(candidates[i])) {
      g_pcaAddr = candidates[i];
      g_linkOk  = true;
      break;
    }
  }

  Serial.print(F("[3/4] 驱动板: "));
  if (g_linkOk) {
    uint8_t mode1    = i2cRead8(g_pcaAddr, PCA_MODE1);
    uint8_t prescale = i2cRead8(g_pcaAddr, PCA_PRESCALE);
    uint32_t freq    = 25000000UL / (4096UL * ((uint32_t)prescale + 1UL));
    pf("在线   地址 0x%02X   MODE1=0x%02X   预分频=%u (~%lu Hz)\n",
       g_pcaAddr, mode1, prescale, (unsigned long)freq);

    /* 给驱动板一个可用的干净状态, PWM 频率设成厂家库默认的 1600Hz */
    pcaAllOff(g_pcaAddr);
    pcaSetFreq(g_pcaAddr, 1600);
    Serial.println(F("      PCA9685 寄存器写入/回读一致, PWM 频率已设为 1600Hz"));
  } else {
    /* 区分"总线就不通"和"总线通了但没有驱动板" */
    if (g_scanCount == 0 && g_timeoutCount > 0 && g_nackCount == 0) {
      pf("总线超时 (%u 次): 驱动板很可能没插好/没供电, 或者 SDA/SCL 引脚不对 (%s)",
         g_timeoutCount, DEFAULT_I2C_HINT);
    } else if (g_scanCount == 0) {
      pf("总线上没有任何器件: 驱动板未连接, 或 I2C 引脚不是 %s", DEFAULT_I2C_HINT);
    } else {
      pf("总线上有 %u 个器件, 但没有一个像 PCA9685: 驱动板地址可能被改成非标准值",
         g_scanCount);
    }
    strncpy(g_msg, "shield not detected", sizeof(g_msg) - 1);
  }

  /* 4. 电机电源提示 */
  Serial.println(F("[4/4] 电机 : I2C 通只代表驱动板逻辑部分有电;"));
  Serial.println(F("            电机要转还必须接 6~12V 动力电源(VM), 且电机接到 M1~M4 接线柱"));

  Serial.println(F("------------------------------------------------------------"));
  if (g_linkOk) {
    Serial.println(F("  结论: 开发板 <-> 电机驱动板   链路正常 (PASS)"));
    Serial.println(F("  下一步: 用 TEST ALL 逐路点动确认电机接线; 然后烧录 MotorDriver"));
  } else {
    Serial.println(F("  结论: 未能确认驱动板 (FAIL)"));
    Serial.println(F("  排查: 1) 驱动板是否完全插到底    2) 板上电源灯是否亮"));
    Serial.println(F("        3) 电池是否接到驱动板 DC 口 4) 必要时 I2C <sda> <scl> 换引脚"));
  }
  Serial.println(F("============================================================"));

#if SENSE_PIN >= 0
  pf("# 电流采样已启用: 引脚 %d, %d mV/A\n", SENSE_PIN, (int)SENSE_MV_PER_A);
#else
  Serial.println(F("# 电流采样未启用 (SENSE_PIN=-1), 点动测试需要人工确认电机"));
#endif

  /* 给上位机解析用的一行 JSON (接在 "#JSON " 后面) */
  pf("#JSON {\"fw\":\"%s\",\"ver\":\"%s\",\"board\":\"%s\","
     "\"sda\":%d,\"scl\":%d,\"scanCount\":%u,\"nack\":%u,\"timeout\":%u,"
     "\"shield\":%s,\"addr\":%u,\"sense\":%s,\"link\":\"%s\"}\n",
     FW_NAME, FW_VERSION, BOARD_NAME,
     g_sda, g_scl, g_scanCount, g_nackCount, g_timeoutCount,
     g_linkOk ? "true" : "false", g_pcaAddr, SENSE_PIN >= 0 ? "true" : "false",
     g_linkOk ? "PASS" : "FAIL");
}

/* ---------------------------------------------------------------------------
 *  串口命令
 * ------------------------------------------------------------------------- */
static void printHelp() {
  Serial.println(F("命令: CHECK | SCAN | TEST ALL | TEST <1-4> | M <1-4> <-255..255>"));
  Serial.println(F("      STOP | I2C <sda> <scl> | HELP"));
}

static void handleCommand(char *line) {
  while (*line == ' ' || *line == '\t') line++;
  int n = strlen(line);
  while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n' || line[n - 1] == ' ')) line[--n] = 0;
  if (n == 0) return;

  char up[64];
  strncpy(up, line, sizeof(up) - 1);
  up[sizeof(up) - 1] = 0;
  for (int i = 0; up[i]; i++) up[i] = toupper(up[i]);

  if (strncmp(up, "CHECK", 5) == 0) {
    runCheck();

  } else if (strncmp(up, "SCAN", 4) == 0) {
    i2cScan();
    pf("# 扫描到 %u 个器件:", g_scanCount);
    for (uint8_t i = 0; i < g_scanCount && i < sizeof(g_scanAddrs); i++)
      pf(" 0x%02X", g_scanAddrs[i]);
    pf("   (NACK=%u TIMEOUT=%u)\n", g_nackCount, g_timeoutCount);

  } else if (strncmp(up, "STOP", 4) == 0) {
    motorStopAll();
    Serial.println(F("# 全部电机已停止"));

  } else if (strncmp(up, "TEST", 4) == 0) {
    if (!g_linkOk) { Serial.println(F("! 驱动板不在线, 请先 CHECK")); return; }
    if (strstr(up, "ALL")) {
      for (uint8_t m = 1; m <= 4; m++) testMotor(m);
    } else {
      int m = atoi(line + 4);
      if (m >= 1 && m <= 4) testMotor((uint8_t)m);
      else Serial.println(F("! 用法: TEST <1-4> 或 TEST ALL"));
    }

  } else if (up[0] == 'M' && (up[1] == ' ' || up[1] == '\t')) {
    if (!g_linkOk) { Serial.println(F("! 驱动板不在线, 请先 CHECK")); return; }
    int m = 0, s = 0;
    if (sscanf(line + 1, "%d %d", &m, &s) == 2 && m >= 1 && m <= 4) {
      if (s < -255) s = -255;
      if (s > 255)  s = 255;
      motorRun((uint8_t)m, s);
      pf("# M%d = %d\n", m, s);
    } else {
      Serial.println(F("! 用法: M <1-4> <-255..255>"));
    }

  } else if (strncmp(up, "I2C", 3) == 0) {
    int sda = 0, scl = 0;
    if (sscanf(line + 3, "%d %d", &sda, &scl) == 2) {
      motorStopAll();
      Wire.end();
      g_wireReady = false;
      g_sda = sda; g_scl = scl;
      g_pcaAddr = 0; g_linkOk = false;
      pf("# I2C 引脚改为 SDA=%d SCL=%d, 重新自检\n", sda, scl);
      runCheck();
    } else {
      Serial.println(F("! 用法: I2C <sda> <scl>"));
    }

  } else if (strncmp(up, "HELP", 4) == 0 || up[0] == '?') {
    printHelp();

  } else {
    Serial.println(F("! 未知命令, 输入 HELP 查看帮助"));
  }
}

/* ---------------------------------------------------------------------------
 *  setup / loop
 * ------------------------------------------------------------------------- */
static char    g_line[80];
static uint8_t g_lineLen = 0;

void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(300);                       // 等 USB 串口稳定
  Serial.println();
  Serial.println(F("MotorLinkCheck 已启动, 正在自检..."));
  printHelp();
  runCheck();                       // 开机自动自检一次
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (g_lineLen > 0) {
        g_line[g_lineLen] = 0;
        handleCommand(g_line);
        g_lineLen = 0;
      }
    } else if (g_lineLen < sizeof(g_line) - 1) {
      g_line[g_lineLen++] = c;
    }
  }
}
