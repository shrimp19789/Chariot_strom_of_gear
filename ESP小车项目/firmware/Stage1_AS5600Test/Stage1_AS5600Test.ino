/*
 * ============================================================================
 *  Stage1_AS5600Test  —  第一阶段：AS5600 磁编码器测试 + I2C 电气诊断
 * ============================================================================
 *
 *  依据: ESP小车项目\ESP32-2804-FOC-guide.md  第 4 节
 *
 *  ★ 重要安全前提 ★
 *     请【断开驱动板功率电源】(那根 8.2~24V 输入), 只留 USB 给 ESP32 供电。
 *     本程序不驱动电机, 且会把 EN 拉低。
 *     按 DRV8313 数据手册, 板上 EN = nSLEEP, 拉低 = 睡眠 = 三相输出高阻,
 *     所以这是安全状态。
 *
 *     ！！但要注意一个坑 ！！
 *     如果编码器的 VCC 是从【降压模块】取的, 而降压模块的输入又来自驱动板,
 *     那么断开驱动功率 = 编码器也断电 = 永远不会应答。
 *     guide 里"核对编码器仍有电"说的就是这个。编码器 VCC 应接 ESP32 的 3.3V。
 *
 * ---------------------------------------------------------------------------
 *  诊断能力 (比单纯"读角度"强在哪)
 * ---------------------------------------------------------------------------
 *  1) 电气层: 在 Wire.begin() 之前把 SDA/SCL 当普通输入读一次(不启用内部上拉)
 *       - 两个都读到高 -> 总线上有【外部上拉电阻】
 *         = 编码器模块很可能已接上并且有电 (AS5600 模块自带 4.7k 上拉)
 *       - 读不到高 -> 没有外部上拉 -> 模块没接上 / 没供电 / 引脚不对
 *  2) 协议层: 全总线扫描, 区分"总线不通"和"有器件但地址不对"
 *  3) 支持换引脚重试: 用串口命令 P <sda> <scl> 换一对引脚重新扫描
 *     (引脚是程序自由指定的, 所以 21/22 只是最可能的猜测, 需要验证)
 *
 * ---------------------------------------------------------------------------
 *  串口命令 (115200)
 * ---------------------------------------------------------------------------
 *   P <sda> <scl>   换一对 I2C 引脚并重新诊断 (例: P 21 22)
 *   S               重新做一次 I2C 扫描
 *   H               帮助
 *
 *  找对引脚后, 每秒打印: raw / deg / MD / ML / MH / changes
 *
 * ============================================================================
 */

#include <Wire.h>

/* ---------------------------------------------------------------------------
 *  引脚配置
 * ---------------------------------------------------------------------------
 *  25/26/27/14 不是猜的: 板上此前烧录的 "2804 OPEN-LOOP MOTOR TEST" 固件
 *  自报 "wiring: IN1/2/3 -> 25/26/27 , EN -> 14"。
 *  SDA/SCL 的 21/22 是 ESP32 默认值, 属于"最可能的猜测", 需要用诊断结果验证。
 */
int SDA_PIN = 21;
int SCL_PIN = 22;
constexpr int EN_PIN = 14;

constexpr uint8_t AS5600_ADDR  = 0x36;
constexpr uint8_t REG_STATUS   = 0x0B;   // bit5=MD 磁铁, bit4=ML 弱, bit3=MH 强
constexpr uint8_t REG_RAW_H    = 0x0C;

/* ---------------------------------------------------------------------------
 *  状态
 * ------------------------------------------------------------------------- */
bool     g_sensorOk   = false;
bool     g_watch      = false;   // 连续监视模式: 一边晃动/重插线, 一边看结果
uint16_t g_lastRaw    = 0;
uint32_t g_changes    = 0;
uint32_t g_lastReport = 0;

/* ---------------------------------------------------------------------------
 *  读寄存器
 * ------------------------------------------------------------------------- */
bool readRegs(uint8_t reg, uint8_t *out, uint8_t count) {
  Wire.beginTransmission(AS5600_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(AS5600_ADDR, count) != count) return false;
  for (uint8_t i = 0; i < count; ++i) out[i] = Wire.read();
  return true;
}

/* ---------------------------------------------------------------------------
 *  电气层诊断: 总线上到底有没有上拉?
 *  在 Wire 接管引脚之前调用。
 * ------------------------------------------------------------------------- */
void electricalCheck() {
  pinMode(SDA_PIN, INPUT);      // 故意不用 INPUT_PULLUP
  pinMode(SCL_PIN, INPUT);
  delay(10);

  int sda = digitalRead(SDA_PIN);
  int scl = digitalRead(SCL_PIN);

  Serial.print(F("  SDA(GPIO")); Serial.print(SDA_PIN);
  Serial.print(F(") 空闲电平=")); Serial.print(sda);
  Serial.print(F("   SCL(GPIO")); Serial.print(SCL_PIN);
  Serial.print(F(") 空闲电平=")); Serial.println(scl);

  if (sda && scl) {
    Serial.println(F("  [电气] 两线都是高 -> 总线上有外部上拉电阻"));
    Serial.println(F("         说明编码器模块很可能已接上并且有电"));
    Serial.println(F("         若此时 0x36 仍不应答, 更可能是地址/芯片问题而不是接线问题"));
  } else {
    Serial.println(F("  [电气] 至少一线不是高 -> 总线上【没有外部上拉】"));
    Serial.println(F("         最可能的原因 (按概率排序):"));
    Serial.println(F("           1) 编码器没有供电 (VCC 接在降压模块上, 而驱动功率被断开了)"));
    Serial.println(F("              -> 编码器 VCC 应接 ESP32 的 3.3V"));
    Serial.println(F("           2) SDA/SCL 不是接在这两个 GPIO 上"));
    Serial.println(F("           3) 编码器 GND 没有和 ESP32 共地"));
    Serial.println(F("           4) 线没插好 / 模块坏了"));
  }
  Serial.println(F("  (提示: 换个引脚组合试试, 命令: P <sda> <scl>)"));
}

/* ---------------------------------------------------------------------------
 *  协议层诊断: 扫总线, 区分错误类型
 * ------------------------------------------------------------------------- */
void scanBus() {
  Serial.println(F("  正在扫描 I2C 总线 (0x08~0x77) ..."));
  uint8_t found = 0;
  uint16_t nack = 0, timeout = 0;

  for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      Serial.print(F("    [器件] 0x"));
      if (addr < 16) Serial.print('0');
      Serial.println(addr, HEX);
      found++;
    } else if (err == 2) nack++;
    else if (err == 5)   timeout++;
    yield();
  }

  Serial.print(F("    结果: 找到 ")); Serial.print(found);
  Serial.print(F(" 个器件 (NACK=")); Serial.print(nack);
  Serial.print(F(" 超时=")); Serial.print(timeout); Serial.println(F(")"));

  if (found == 0) {
    if (timeout > 0 && nack == 0) {
      Serial.println(F("    全是超时 -> 两条线被拉死, 典型的没供电/没上拉"));
    } else {
      Serial.println(F("    有 NACK 说明总线本身是通的, 只是没有器件应答"));
    }
  } else {
    Serial.println(F("    总线是通的。AS5600 应当是 0x36; 若不是, 说明不是 AS5600"));
  }
}

/* ---------------------------------------------------------------------------
 *  完整诊断流程
 * ------------------------------------------------------------------------- */
void diagnose() {
  Serial.println();
  Serial.println(F("============================================================"));
  Serial.print  (F("  Stage 1: AS5600 测试   SDA=GPIO")); Serial.print(SDA_PIN);
  Serial.print  (F("  SCL=GPIO")); Serial.println(SCL_PIN);
  Serial.print  (F("  EN =GPIO")); Serial.print(EN_PIN);
  Serial.println(F(" (已拉低 = 驱动关闭)"));
  Serial.println(F("============================================================"));

  /* 1. 电气层 */
  Serial.println(F("[1/3] 电气检查 (未启用内部上拉)"));
  electricalCheck();

  /* 2. 协议层 */
  Serial.println(F("[2/3] 协议检查"));
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(20);
  delay(50);

  Wire.beginTransmission(AS5600_ADDR);
  if (Wire.endTransmission() == 0) {
    Serial.println(F("    [OK] AS5600 已在 0x36 应答!"));
    g_sensorOk = true;
    Serial.println(F("[3/3] 进入连续读取"));
    Serial.println(F("------------------------------------------------------------"));
    Serial.println(F("  请用手慢慢转动电机一圈, 观察 raw 是否走满 0~4095"));
    Serial.println(F("  列: raw / deg / MD(磁铁) / ML(弱) / MH(强) / changes"));
    Serial.println(F("------------------------------------------------------------"));
    return;
  }

  Serial.println(F("    [FAIL] 0x36 无应答"));
  scanBus();
  g_sensorOk = false;
  Serial.println(F("[3/3] 本阶段无法继续, 请按上面的提示排查"));
  Serial.println(F("      换引脚组合可试: P 18 19 / P 32 33 / P 21 22"));
  Serial.println(F("      >>> 发 W 进入连续监视模式: 一边晃动/重插线, 一边看器件数 <<<"));
  Serial.println(F("      重点检查: 编码器 GND 是否和 ESP32 共地 / 杜邦线是否接触良好"));
  Serial.println(F("============================================================"));
}

void printHelp() {
  Serial.println(F("命令:"));
  Serial.println(F("  W              连续监视(晃动/重插线时用)"));
  Serial.println(F("  G              扫描所有 GPIO, 找出哪个引脚上接了外部上拉电阻"));
  Serial.println(F("  L              低速重试(50kHz/10kHz)+强制内部上拉"));
  Serial.println(F("  P <sda> <scl>  换一对 I2C 引脚重新诊断"));
  Serial.println(F("  S              重新扫描总线   H  帮助"));
}

/* ---------------------------------------------------------------------------
 *  G: 扫描所有 GPIO, 找出哪些引脚上有【外部上拉电阻】
 *
 *  原理: 先启用 ESP32 的内部下拉(约 45k)。如果某个引脚仍然读到高电平,
 *        说明它上面挂着一个比内部下拉强得多的外部上拉电阻。
 *
 *  为什么这个测试有用:
 *        AS5600 模块自带上拉电阻(典型 4.7k)。所以"哪个引脚有外部上拉",
 *        基本就等于"编码器的 SDA/SCL 接在哪两个引脚上"。
 *        这样就不用再猜引脚了。
 *
 *  注意: 会跳过 0/2/12/15 这几个 strapping 引脚, 避免干扰启动条件。
 * ------------------------------------------------------------------------- */
void scanPullups() {
  Serial.println();
  Serial.println(F("=== 扫描外部上拉电阻 (含强度测量) ==="));

  /* ★ 必须先把 I2C 外设从引脚上摘下来 ★
   * 否则 GPIO21/22 仍挂在 I2C 外设上, 而 ESP32 对被外设占用的引脚
   * 调用 pinMode() 是不可靠的(GPIO 矩阵路由会压过引脚配置)。
   * 之前那次"GPIO21 没有上拉"的误判就漏了这一步。 */
  Wire.end();
  delay(20);

  const int pins[] = { 4, 5, 13, 14, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33 };
  const int nPins = sizeof(pins) / sizeof(pins[0]);

  Serial.println(F("方法: 先把线拉低, 再放开(不加上拉), 量它自己回升到高需要多久"));
  Serial.println(F("      上升越快 = 上拉越强; 一直不回升 = 真的没有上拉"));
  Serial.println(F("      <5us=强(4.7k级) 5~50us=中 50~500us=弱(100k级) 不回升=无"));
  Serial.println();

  for (int i = 0; i < nPins; i++) {
    int p = pins[i];

    /* 1. 拉低 */
    pinMode(p, OUTPUT);
    digitalWrite(p, LOW);
    delay(5);
    int lowOk = (digitalRead(p) == 0);

    /* 2. 放开, 不启用任何内部上下拉, 量上升时间 */
    pinMode(p, INPUT);
    uint32_t t0 = micros();
    uint32_t rise = 0;
    while (micros() - t0 < 3000) {          // 最多等 3ms
      if (digitalRead(p)) { rise = micros() - t0; break; }
    }

    /* 3. 再用内部下拉(约45k)压一下, 判断强不强 */
    pinMode(p, INPUT_PULLDOWN);
    delay(3);
    int strong = digitalRead(p);

    if (rise > 0 || strong) {
      Serial.print(F("  GPIO")); Serial.print(p);
      Serial.print(F("  上升="));
      if (rise == 0) Serial.print(F("未回升(>3ms)"));
      else { Serial.print(rise); Serial.print(F("us")); }
      Serial.print(F("  内部下拉后="));
      Serial.print(strong ? F("高(强上拉)") : F("低(弱上拉)"));
      if (!lowOk) Serial.print(F("  [警告: 拉不低!]"));
      Serial.println();
    }
  }

  /* 恢复 EN 为输出低, 保证驱动保持关闭 */
  pinMode(EN_PIN, OUTPUT);
  digitalWrite(EN_PIN, LOW);

  Serial.println();
  Serial.println(F("判读: 只要某个引脚'上升'有数值, 就说明它有上拉(不管强弱)。"));
  Serial.println(F("      万用表能测到弱上拉, 而本测试的'内部下拉'会把弱上拉读成低 ——"));
  Serial.println(F("      两者不矛盾, 测的是不同东西(存在 vs 强度)。"));

  /* 扫完把 I2C 恢复起来 */
  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(20);
  Serial.println(F("==================================="));
}

/* ---------------------------------------------------------------------------
 *  L: 低速重试。如果上拉电阻偏弱, 下降沿能被拉低但上升沿回不去,
 *     扫描就会全是 NACK。降低速率 + 并联内部上拉可以验证这一点。
 * ------------------------------------------------------------------------- */
void slowRetry() {
  const uint32_t speeds[] = { 50000, 10000 };
  Serial.println();
  Serial.println(F("=== 低速重试 (并联内部上拉) ==="));

  for (uint8_t i = 0; i < 2; i++) {
    Wire.end();
    delay(20);
    pinMode(SDA_PIN, INPUT_PULLUP);      // 与外部上拉并联, 增强驱动能力
    pinMode(SCL_PIN, INPUT_PULLUP);
    Wire.begin(SDA_PIN, SCL_PIN);
    Wire.setClock(speeds[i]);
    Wire.setTimeOut(20);
    delay(50);

    Serial.print(F("  ")); Serial.print(speeds[i] / 1000);
    Serial.print(F(" kHz: "));

    Wire.beginTransmission(AS5600_ADDR);
    if (Wire.endTransmission() == 0) {
      Serial.println(F("*** AS5600 应答了! 说明之前是上拉太弱 ***"));
      g_sensorOk = true;
      return;
    }

    uint8_t found = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) {
        Serial.print(F("找到 0x"));
        if (a < 16) Serial.print('0');
        Serial.print(a, HEX);
        Serial.print(' ');
        found++;
      }
      yield();
    }
    if (found == 0) Serial.println(F("0x36 无应答, 总线上也没有任何器件"));
    else Serial.println();
  }
  Serial.println(F("结论: 低速也不行 -> 问题不在速率/上拉强度"));
  Serial.println(F("==============================="));
}

/* ---------------------------------------------------------------------------
 *  连续监视: 每秒扫一次, 让你一边拨弄线一边看结果
 *  发现器件就立刻报出来, 并自动切到读数模式。
 * ------------------------------------------------------------------------- */
bool watchOnce() {
  /* ★ 先把 I2C 外设摘下来再读电平 ★
   * 之前这里直接 pinMode(INPUT) 就读, 那时引脚还挂在 I2C 外设上,
   * 读出来的"空闲电平"并不可靠(会受引脚残留电荷/外设状态影响)。
   * 现在分三次测, 结果才有意义:
   *   自由电平    : 不启用任何内部上下拉, 看真实线上的电平
   *   内部下拉后  : 启用 45k 下拉, 强的外部上拉仍会读到高
   */
  Wire.end();
  delay(5);

  pinMode(SDA_PIN, INPUT);
  pinMode(SCL_PIN, INPUT);
  delay(3);
  int sdaFree = digitalRead(SDA_PIN);
  int sclFree = digitalRead(SCL_PIN);

  pinMode(SDA_PIN, INPUT_PULLDOWN);
  pinMode(SCL_PIN, INPUT_PULLDOWN);
  delay(3);
  int sdaPD = digitalRead(SDA_PIN);
  int sclPD = digitalRead(SCL_PIN);

  Wire.begin(SDA_PIN, SCL_PIN);
  Wire.setClock(100000);
  Wire.setTimeOut(10);

  uint8_t found = 0;
  uint8_t addrs[8];
  for (uint8_t a = 0x08; a <= 0x77; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      if (found < 8) addrs[found] = a;
      found++;
    }
    yield();
  }

  Serial.print(F("  监视: SDA 自由=")); Serial.print(sdaFree);
  Serial.print(F(" 下拉后=")); Serial.print(sdaPD);
  Serial.print(F(" | SCL 自由=")); Serial.print(sclFree);
  Serial.print(F(" 下拉后=")); Serial.print(sclPD);
  Serial.print(F(" | 器件数=")); Serial.print(found);

  if (found) {
    Serial.print(F("  -> "));
    for (uint8_t i = 0; i < found && i < 8; i++) {
      Serial.print(F("0x"));
      if (addrs[i] < 16) Serial.print('0');
      Serial.print(addrs[i], HEX);
      Serial.print(' ');
    }
    Serial.println();
    // 是不是我们要的 AS5600?
    for (uint8_t i = 0; i < found && i < 8; i++) {
      if (addrs[i] == AS5600_ADDR) {
        Serial.println(F("  *** 找到 AS5600! 接线通了 ***"));
        return true;
      }
    }
  } else {
    Serial.println();
  }
  return false;
}

/* ---------------------------------------------------------------------------
 *  setup / loop
 * ------------------------------------------------------------------------- */
void setup() {
  pinMode(EN_PIN, OUTPUT);
  digitalWrite(EN_PIN, LOW);      // 先确保驱动关闭

  Serial.begin(115200);
  delay(600);
  printHelp();
  diagnose();
}

void loop() {
  /* ---- 串口命令 ---- */
  static char line[32];
  static uint8_t len = 0;

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (len) {
        line[len] = 0;
        char up = toupper(line[0]);
        if (up == 'P') {
          int sda = 0, scl = 0;
          if (sscanf(line + 1, "%d %d", &sda, &scl) == 2) {
            Wire.end();
            SDA_PIN = sda; SCL_PIN = scl;
            g_sensorOk = false;
            Serial.print(F(">> 改用 SDA=")); Serial.print(sda);
            Serial.print(F(" SCL=")); Serial.println(scl);
            diagnose();
          } else {
            Serial.println(F("! 用法: P <sda> <scl>   例: P 21 22"));
          }
        } else if (up == 'W') {
          g_watch = !g_watch;
          g_sensorOk = false;      // 进入监视前先退出读数模式
          if (g_watch) {
            Serial.println(F(">> 进入连续监视模式 (每秒一次)"));
            Serial.println(F("   现在可以轻轻晃动 / 重插编码器的 VCC、GND、SDA、SCL 线,"));
            Serial.println(F("   观察上面的'器件数'有没有变化。再发一次 W 退出。"));
          } else {
            Serial.println(F(">> 退出连续监视模式"));
          }
        } else if (up == 'G') {
          g_watch = false;
          scanPullups();

        } else if (up == 'L') {
          g_watch = false;
          slowRetry();

        } else if (up == 'S') {
          scanBus();
        } else if (up == 'H') {
          printHelp();
        } else {
          Serial.println(F("! 未知命令"));
          printHelp();
        }
        len = 0;
      }
    } else if (len < sizeof(line) - 1) {
      line[len++] = c;
    }
  }

  /* ---- 连续监视模式 ---- */
  if (g_watch) {
    static uint32_t lastWatch = 0;
    if (millis() - lastWatch >= 1000) {
      lastWatch = millis();
      if (watchOnce()) {
        g_watch = false;
        g_sensorOk = true;
        Serial.println(F("   已自动切换到读数模式。请用手慢慢转动电机一圈。"));
      }
    }
    return;
  }

  /* ---- 读数 ---- */
  if (!g_sensorOk) { delay(500); return; }

  uint8_t status = 0, angle[2] = {0, 0};
  if (!readRegs(REG_STATUS, &status, 1) || !readRegs(REG_RAW_H, angle, 2)) {
    Serial.println(F("  I2C ERROR: 读取失败, 检查接线"));
    delay(500);
    return;
  }

  uint16_t raw = ((uint16_t(angle[0]) << 8) | angle[1]) & 0x0FFF;
  if (raw != g_lastRaw) { g_changes++; g_lastRaw = raw; }

  if (millis() - g_lastReport >= 200) {
    g_lastReport = millis();
    Serial.print(F("raw="));   Serial.print(raw);
    Serial.print(F("  deg=")); Serial.print(raw * 360.0f / 4096.0f, 2);
    Serial.print(F("  MD="));  Serial.print((status & 0x20) != 0);
    Serial.print(F("  ML="));  Serial.print((status & 0x10) != 0);
    Serial.print(F("  MH="));  Serial.print((status & 0x08) != 0);
    Serial.print(F("  changes=")); Serial.println(g_changes);
  }

  delay(20);
}
