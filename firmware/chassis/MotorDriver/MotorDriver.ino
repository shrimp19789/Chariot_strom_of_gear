/*
 * ============================================================================
 *  MotorDriver  —  ESP32 + 奇果派(QGPMaker) 电机驱动板  简易电机驱动程序
 * ============================================================================
 *
 *  这是"真正干活"的固件: 用厂家官方库 QGPMaker_MotorShield 驱动 4 路直流电机
 *  和 8 路舵机, 并对外提供两套控制协议 —— 一套给人/给 Python 用的 ASCII 文本协议,
 *  一套兼容厂家 UartJoystick 例程的 14 字节二进制协议。
 *
 *  ★ 上位机链路检测请配合 MotorLinkCheck 固件, 或直接用 host/motor_link_check.py
 *
 * ---------------------------------------------------------------------------
 *  一、ASCII 文本协议 (115200 8N1, 每行一条命令, 以 \n 结束)
 * ---------------------------------------------------------------------------
 *   M <1-4> <-255..255>   设置某路电机速度。正数正转, 负数反转, 0 停止(释放)
 *   STOP                  四路电机全部停止(释放), 舵机保持不动
 *   BRK <1-4>             某路电机刹车急停(短接线圈, 大功率电机慎用)
 *   DRIVE <左> <右>       差速驱动: 左 = M1+M2, 右 = M3+M4, 取值 -255..255
 *   SERVO <0-7> <0-180>   设置舵机角度
 *   WD <毫秒>             看门狗: 超过该时间没收到任何命令就自动停车; 0 = 关闭
 *   PING                  握手, 回 PONG (用于检测板子是否在线)
 *   STATUS                回一行 JSON 状态
 *   HELP                  帮助
 *
 *   例:  printf 'M 1 200\nM 2 200\n' > /dev/ttyUSB0    (Linux)
 *        M 1 -150    -> M1 反转 150
 *
 * ---------------------------------------------------------------------------
 *  二、二进制协议 (兼容厂家 UartJoystick 例程, 共 14 字节)
 * ---------------------------------------------------------------------------
 *   字节 0-1 : 固定头 0x01 0xFF
 *   字节 2-3 : M1 速度, int16 大端, -255..255
 *   字节 4-5 : M2 速度
 *   字节 6-7 : M3 速度
 *   字节 8-9 : M4 速度
 *   字节 10  : 舵机1 角度 0-180
 *   字节 11  : 舵机2 角度
 *   字节 12  : 舵机3 角度
 *   字节 13  : 舵机4 角度
 *
 * ---------------------------------------------------------------------------
 *  三、硬件前提
 * ---------------------------------------------------------------------------
 *   - 驱动板默认 I2C 地址 0x60 (V5.6 支持改地址, 若改过请修改 SHIELD_I2C_ADDR)
 *   - ESP32 经典款默认 I2C: SDA=GPIO21, SCL=GPIO22
 *   - 电机要转, 必须给驱动板接 6~12V 动力电源(VM), 光靠 USB 供电不够
 *   - 电机接在 M1~M4 接线柱上; 转向反了就对调该路电机的两根线
 *
 * ============================================================================
 */

#include <Wire.h>
#include <stdarg.h>
#include "QGPMaker_MotorShield.h"

#define FW_NAME    "MotorDriver"
#define FW_VERSION "1.0"

#define SERIAL_BAUD        115200
#define SHIELD_I2C_ADDR    0x60     // 驱动板 I2C 地址 (V5.2 固定 0x60)
#define SHIELD_PWM_FREQ    1600     // 厂家库默认 1.6kHz; 用舵机建议改 50
#define DEFAULT_WATCHDOG_MS 3000    // 默认 3 秒无命令自动停车; 0 = 关闭

/* 差速驱动的左右轮分组 (按实际小车接线调整) */
#define DRIVE_LEFT_MOTOR_A   1
#define DRIVE_LEFT_MOTOR_B   2
#define DRIVE_RIGHT_MOTOR_A  3
#define DRIVE_RIGHT_MOTOR_B  4

/* 车体方向取反开关: 若某个轮子转向装反了又不方便改线, 把对应项改成 -1 */
static const int MOTOR_INVERT[5] = { 0, 1, 1, 1, 1 };   // 下标 1..4

QGPMaker_MotorShield AFMS = QGPMaker_MotorShield(SHIELD_I2C_ADDR);

/* ESP32 的 HardwareSerial 有 printf, AVR 没有, 所以统一走这个函数 */
static void pf(const char *fmt, ...) {
  char buf[220];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  Serial.print(buf);
}

static QGPMaker_DCMotor *motor[5];     // 下标 1..4
static QGPMaker_Servo   *servo[8];     // 下标 0..7
static int  g_motorSpeed[5] = { 0, 0, 0, 0, 0 };
static int  g_servoAngle[8] = { 90, 90, 90, 90, 90, 90, 90, 90 };

static bool     g_shieldPresent = false;
static uint32_t g_watchdogMs    = DEFAULT_WATCHDOG_MS;
static uint32_t g_lastCmdMs     = 0;
static bool     g_moving        = false;

/* ---------------------------------------------------------------------------
 *  启动前先确认驱动板在不在总线上 (用裸 Wire, 不依赖库)
 * ------------------------------------------------------------------------- */
static bool shieldProbe() {
  Wire.beginTransmission(SHIELD_I2C_ADDR);
  if (Wire.endTransmission() != 0) return false;

  /* 写图案 + 回读, 确认真的是 PCA9685 而不是别的同地址器件 */
  Wire.beginTransmission(SHIELD_I2C_ADDR);
  Wire.write(0x00);            // MODE1
  Wire.write(0x20);            // 自动递增
  if (Wire.endTransmission() != 0) return false;

  Wire.beginTransmission(SHIELD_I2C_ADDR);
  Wire.write(0x00);
  if (Wire.endTransmission() != 0) return false;
  if (Wire.requestFrom(SHIELD_I2C_ADDR, (uint8_t)1) != 1) return false;
  uint8_t rb = (uint8_t)Wire.read();
  return (rb & 0x20) != 0;
}

/* ---------------------------------------------------------------------------
 *  电机控制
 * ------------------------------------------------------------------------- */
static void setMotor(uint8_t m, int speed) {
  if (m < 1 || m > 4 || motor[m] == NULL) return;

  if (speed > 255)  speed = 255;
  if (speed < -255) speed = -255;
  speed *= MOTOR_INVERT[m];

  /* 必须先 run() 定方向, 再 setSpeed() —— 库内部 setSpeed 会用上一次的方向重发 */
  if (speed >= 0) {
    motor[m]->run(FORWARD);
    motor[m]->setSpeed((uint8_t)speed);
  } else {
    motor[m]->run(BACKWARD);
    motor[m]->setSpeed((uint8_t)(-speed));
  }
  g_motorSpeed[m] = speed;
}

static void stopAll() {
  for (uint8_t m = 1; m <= 4; m++) {
    if (motor[m]) {
      motor[m]->setSpeed(0);
      motor[m]->run(RELEASE);
    }
    g_motorSpeed[m] = 0;
  }
  g_moving = false;
}

static void brakeMotor(uint8_t m) {
  if (m < 1 || m > 4 || motor[m] == NULL) return;
  motor[m]->run(BRAKE);
  g_motorSpeed[m] = 0;
}

static void setServo(uint8_t n, int angle) {
  if (n > 7 || servo[n] == NULL) return;
  if (angle < 0)   angle = 0;
  if (angle > 180) angle = 180;
  servo[n]->writeServo((uint8_t)angle);
  g_servoAngle[n] = angle;
}

static void drive(int left, int right) {
  setMotor(DRIVE_LEFT_MOTOR_A,  left);
  setMotor(DRIVE_LEFT_MOTOR_B,  left);
  setMotor(DRIVE_RIGHT_MOTOR_A, right);
  setMotor(DRIVE_RIGHT_MOTOR_B, right);
}

/* ---------------------------------------------------------------------------
 *  状态输出
 * ------------------------------------------------------------------------- */
static void printStatus() {
  pf("#STATUS {\"fw\":\"%s\",\"ver\":\"%s\",\"shield\":%s,\"addr\":%d,"
                "\"m1\":%d,\"m2\":%d,\"m3\":%d,\"m4\":%d,"
                "\"s0\":%d,\"s1\":%d,\"s2\":%d,\"s3\":%d,\"wd\":%lu,\"up\":%lu}\n",
                FW_NAME, FW_VERSION,
                g_shieldPresent ? "true" : "false", SHIELD_I2C_ADDR,
                g_motorSpeed[1], g_motorSpeed[2], g_motorSpeed[3], g_motorSpeed[4],
                g_servoAngle[0], g_servoAngle[1], g_servoAngle[2], g_servoAngle[3],
                (unsigned long)g_watchdogMs, (unsigned long)(millis() / 1000));
}

static void printHelp() {
  Serial.println(F("-----------------------------------------------------------"));
  Serial.println(F(" 简易电机驱动 MotorDriver 已就绪, 可用命令:"));
  Serial.println(F("   M <1-4> <-255..255>   驱动某路电机 (正=正转 负=反转 0=停)"));
  Serial.println(F("   STOP                  全部停止       BRK <1-4>  单路刹车"));
  Serial.println(F("   DRIVE <左> <右>       差速驱动 (左=M1+M2, 右=M3+M4)"));
  Serial.println(F("   SERVO <0-7> <0-180>   设置舵机角度"));
  Serial.println(F("   WD <毫秒>             看门狗超时自动停车 (0=关闭)"));
  Serial.println(F("   PING / STATUS / HELP"));
  Serial.println(F("  也支持厂家 14 字节二进制协议: 01 FF + 4x int16 + 4x uint8"));
  Serial.println(F("-----------------------------------------------------------"));
}

/* ---------------------------------------------------------------------------
 *  命令解析
 * ------------------------------------------------------------------------- */
static void handleAscii(char *line) {
  while (*line == ' ' || *line == '\t') line++;
  int n = strlen(line);
  while (n > 0 && (line[n - 1] == ' ' || line[n - 1] == '\t')) line[--n] = 0;
  if (n == 0) return;

  char up[64];
  strncpy(up, line, sizeof(up) - 1);
  up[sizeof(up) - 1] = 0;
  for (int i = 0; up[i]; i++) up[i] = toupper(up[i]);

  g_lastCmdMs = millis();

  if (strncmp(up, "PING", 4) == 0) {
    pf("PONG %s %s shield=%d\n", FW_NAME, FW_VERSION, g_shieldPresent ? 1 : 0);

  } else if (strncmp(up, "STATUS", 6) == 0) {
    printStatus();

  } else if (strncmp(up, "HELP", 4) == 0 || up[0] == '?') {
    printHelp();

  } else if (strncmp(up, "STOP", 4) == 0) {
    stopAll();
    Serial.println(F("OK STOP"));

  } else if (strncmp(up, "BRK", 3) == 0) {
    int m = atoi(line + 3);
    if (m >= 1 && m <= 4) { brakeMotor((uint8_t)m); pf("OK BRK %d\n", m); }
    else Serial.println(F("ERR usage: BRK <1-4>"));

  } else if (strncmp(up, "DRIVE", 5) == 0) {
    int l = 0, r = 0;
    if (sscanf(line + 5, "%d %d", &l, &r) == 2) {
      drive(l, r);
      g_moving = (l != 0 || r != 0);
      pf("OK DRIVE %d %d\n", l, r);
    } else Serial.println(F("ERR usage: DRIVE <left> <right>"));

  } else if (strncmp(up, "SERVO", 5) == 0) {
    int s = 0, a = 0;
    if (sscanf(line + 5, "%d %d", &s, &a) == 2 && s >= 0 && s <= 7) {
      setServo((uint8_t)s, a);
      pf("OK SERVO %d %d\n", s, a);
    } else Serial.println(F("ERR usage: SERVO <0-7> <0-180>"));

  } else if (strncmp(up, "WD", 2) == 0) {
    long v = atol(line + 2);
    g_watchdogMs = (v <= 0) ? 0 : (uint32_t)v;
    pf("OK WD %lu\n", (unsigned long)g_watchdogMs);

  } else if (up[0] == 'M' && (up[1] == ' ' || up[1] == '\t')) {
    int m = 0, s = 0;
    if (sscanf(line + 1, "%d %d", &m, &s) == 2 && m >= 1 && m <= 4) {
      setMotor((uint8_t)m, s);
      g_moving = (g_motorSpeed[1] || g_motorSpeed[2] || g_motorSpeed[3] || g_motorSpeed[4]);
      pf("OK M %d %d\n", m, g_motorSpeed[m]);
    } else Serial.println(F("ERR usage: M <1-4> <-255..255>"));

  } else {
    pf("ERR unknown command: %s\n", line);
  }
}

/* 厂家 14 字节二进制帧 */
static void handleBinary(const uint8_t *b) {
  if (b[0] != 0x01 || b[1] != 0xFF) return;

  int16_t spd[4];
  for (int i = 0; i < 4; i++)
    spd[i] = (int16_t)((b[2 + i * 2] << 8) | b[3 + i * 2]);

  for (int i = 0; i < 4; i++) setMotor((uint8_t)(i + 1), spd[i]);
  for (int i = 0; i < 4; i++) setServo((uint8_t)i, b[10 + i]);

  g_lastCmdMs = millis();
  g_moving = (spd[0] || spd[1] || spd[2] || spd[3]);

  pf("#BIN %d,%d,%d,%d %d,%d,%d,%d\n",
                spd[0], spd[1], spd[2], spd[3], b[10], b[11], b[12], b[13]);
}

/* ---------------------------------------------------------------------------
 *  setup
 * ------------------------------------------------------------------------- */
void setup() {
  Serial.begin(SERIAL_BAUD);
  delay(300);

  Serial.println();
  pf("============================================================\n");
  pf("  %s v%s  ——  奇果派 QGPMaker 电机驱动板\n", FW_NAME, FW_VERSION);
  pf("============================================================\n");

  /* 1. 自检: 驱动板在不在 I2C 总线上 */
  Wire.begin();
  delay(50);
  g_shieldPresent = shieldProbe();

  if (g_shieldPresent) {
    pf("[OK]   驱动板在线 (I2C 地址 0x%02X), 寄存器回读一致\n", SHIELD_I2C_ADDR);
  } else {
    pf("[FAIL] I2C 地址 0x%02X 无应答 —— 驱动板未连接或未供电\n", SHIELD_I2C_ADDR);
    Serial.println(F("       仍会继续运行, 但电机命令不会生效。"));
    Serial.println(F("       请先烧录 MotorLinkCheck 固件做完整排查。"));
  }

  /* 2. 初始化驱动板与全部电机/舵机 */
  AFMS.begin(SHIELD_PWM_FREQ);

  for (uint8_t m = 1; m <= 4; m++) {
    motor[m] = AFMS.getMotor(m);
    if (motor[m]) { motor[m]->setSpeed(0); motor[m]->run(RELEASE); }
  }
  for (uint8_t s = 0; s < 8; s++) {
    servo[s] = AFMS.getServo(s);
    if (servo[s]) servo[s]->writeServo(90);   // 舵机归中, 避免上电乱甩
  }

  g_lastCmdMs = millis();
  printHelp();
  printStatus();
  Serial.println(F("READY"));
}

/* ---------------------------------------------------------------------------
 *  loop
 * ------------------------------------------------------------------------- */
static char    asciiBuf[64];
static uint8_t asciiLen  = 0;
static uint8_t binBuf[14];
static uint8_t binLen    = 0;
static bool    inBinary  = false;
static uint32_t binStart = 0;

void loop() {
  /* ---- 收命令 ---- */
  while (Serial.available()) {
    /* 第一个字节是 0x01 就认为是二进制帧的开头 */
    if (!inBinary) {
      if (Serial.peek() == 0x01) { inBinary = true; binLen = 0; binStart = millis(); }
    }

    if (inBinary) {
      binBuf[binLen++] = (uint8_t)Serial.read();

      if (binLen == 2 && binBuf[1] != 0xFF) {     // 不是合法帧头, 放弃
        inBinary = false; binLen = 0;
        continue;
      }
      if (binLen == 14) {
        handleBinary(binBuf);
        inBinary = false; binLen = 0;
      }
    } else {
      char c = (char)Serial.read();
      if (c == '\n' || c == '\r') {
        if (asciiLen) { asciiBuf[asciiLen] = 0; handleAscii(asciiBuf); asciiLen = 0; }
      } else if (asciiLen < sizeof(asciiBuf) - 1) {
        asciiBuf[asciiLen++] = c;
      }
    }
  }

  /* 二进制帧收一半卡住了就复位, 避免把后面的 ASCII 命令吃掉 */
  if (inBinary && (millis() - binStart > 200)) { inBinary = false; binLen = 0; }

  /* ---- 看门狗 ---- */
  if (g_watchdogMs > 0 && g_moving && (millis() - g_lastCmdMs > g_watchdogMs)) {
    stopAll();
    Serial.println(F("#WATCHDOG 超时, 已自动停止全部电机"));
  }

  /* ---- 周期性状态上报(每 2 秒, 便于上位机确认板子活着) ---- */
  static uint32_t lastBeat = 0;
  if (millis() - lastBeat > 2000) {
    lastBeat = millis();
    pf("#ALIVE shield=%d m=%d,%d,%d,%d\n",
                  g_shieldPresent ? 1 : 0,
                  g_motorSpeed[1], g_motorSpeed[2], g_motorSpeed[3], g_motorSpeed[4]);
  }
}
