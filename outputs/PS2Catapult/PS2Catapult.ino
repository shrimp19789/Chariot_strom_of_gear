/* ESP32 + AS5600 + SimpleFOC：PS2 按住蓄力 / 90度行程 / 自动复位。
 * 本版本为低速调试原型，不保证投掷性能，也不能保证物理上绝不越过90度。
 * 默认禁止电机输出：先解决编码器、供电、硬限位和急停，再修改配置。
 * 首次 c 校准必须卸下投掷杆：SimpleFOC 对齐可能转动超过工作行程。
 * 无电流采样，1V电压限制不是电流保护；软件禁用后机构可能自由下落。
 * PS2接口按通用协议实现，TK-PS2A 的实际兼容性需看串口验证。
 */
#include <Arduino.h>
#include <Wire.h>
#include <SimpleFOC.h>

// ---------- 用户接线 ----------
constexpr int SDA_PIN=21, SCL_PIN=22;
constexpr int IN1=25, IN2=26, IN3=27, EN=14;
constexpr int PS_CLK=2, PS_CS=4, PS_CMD=12, PS_DAT=13;
// GPIO2/12为启动配置脚；外设启动电平可能导致ESP32无法启动/下载。
// 若出现启动问题，建议重新布线到 CLK=18、CMD=23，并同步修改上面参数。

// ---------- 必须人工核对的配置 ----------
constexpr bool CONFIG_CONFIRMED=false;
constexpr float SUPPLY_VOLTAGE=0.0f; // 填驱动板功率输入实测值，不能填逻辑5V。
constexpr int POLE_PAIRS=7;
constexpr float DIRECTION=1.0f; // 只允许 +1或-1，卸杆低速核对正方向。
constexpr float MOTOR_TO_ARM_RATIO=1.0f; // 电机角度/杆角度，直连=1。
constexpr float VOLTAGE_LIMIT=1.0f; // 初次低压测试；不得靠提高此值解决不转。
constexpr float TRAVEL=PI/2;         // 杆的目标行程90度。
constexpr float MIN_SPEED=0.3f;      // 杆角速度rad/s，低速调试默认值。
constexpr float MAX_SPEED=1.5f;      // 有上限；不是已验证的投掷速度。
constexpr float RETURN_SPEED=0.3f;
constexpr float ACCEL=2.0f;          // 杆角加速度rad/s^2。
constexpr uint32_t FULL_CHARGE_MS=3000, MIN_CHARGE_MS=100;
constexpr uint32_t PS_TIMEOUT_MS=150, MOVE_TIMEOUT_MS=15000;
constexpr uint32_t IDLE_TIMEOUT_MS=20000;
constexpr float POSITION_TOL=PI/180; // 到位误差1度，需连续稳定200ms。
constexpr float RANGE_MARGIN=2*PI/180; // 越界故障阈值，不是允许行程。

// 自定义带错误检查的传感器：读不到两个字节就保持缓存并置故障标志。
// 避免在I2C失败时把错误读数当成真实角度继续驱动。
class CheckedAS5600: public Sensor {
 public:
  void init() override { Sensor::init(); }
  bool ok=false;
  float cached=0;
  uint32_t lastGood=0;
  float getSensorAngle() override {
    uint8_t b[2];
    ok=read(0x0C,b,2);
    if(ok) {
      uint16_t raw=((uint16_t(b[0])<<8)|b[1])&0x0FFF;
      cached=raw*(2*PI/4096.0f);
      lastGood=millis();
    }
    return cached;
  }
  bool status(uint8_t &s) { return read(0x0B,&s,1); }
 private:
  bool read(uint8_t reg,uint8_t *out,uint8_t n) {
    Wire.beginTransmission(0x36); Wire.write(reg);
    if(Wire.endTransmission(false)!=0) return false;
    if(Wire.requestFrom(uint8_t(0x36),n)!=n) return false;
    for(uint8_t i=0;i<n;i++) out[i]=Wire.read();
    return true;
  }
};
CheckedAS5600 sensor;
BLDCDriver3PWM driver(IN1,IN2,IN3,EN);
BLDCMotor motor(POLE_PAIRS);

// PS2按钮位：回复中的按钮是低有效，poll时转换成高有效。
constexpr uint16_t CROSS=1u<<14, CIRCLE=1u<<13, L1=1u<<10;
uint16_t buttons=0, previousButtons=0;
uint8_t psMode=0;
uint16_t validFrames=0;
uint32_t lastPS=0, lastPoll=0;
bool psValid=false;

// 通用PS2四线通信：CLK空闲高，CS低选中，CMD/DAT最低位先传。
// 只读取数字按键，不需要震动/摇杆/压力模式，接受41/73/79模式。
uint8_t exchangeByte(uint8_t tx) {
  uint8_t rx=0;
  for(uint8_t bit=0;bit<8;bit++) {
    digitalWrite(PS_CMD,(tx>>bit)&1);
    digitalWrite(PS_CLK,LOW); delayMicroseconds(4);
    if(digitalRead(PS_DAT)) rx|=1u<<bit;
    digitalWrite(PS_CLK,HIGH); delayMicroseconds(4);
  }
  digitalWrite(PS_CMD,HIGH); delayMicroseconds(20);
  return rx;
}
bool pollPS2() {
  uint8_t rx[21]={};
  digitalWrite(PS_CS,LOW); delayMicroseconds(20);
  uint8_t length=9;
  for(uint8_t i=0;i<length;i++) {
    rx[i]=exchangeByte(i==0?0x01:(i==1?0x42:0));
    if(i==1 && rx[1]==0x79) length=21;
  }
  digitalWrite(PS_CS,HIGH);
  psMode=rx[1];
  if(rx[2]!=0x5A || (psMode!=0x41 && psMode!=0x73 && psMode!=0x79)) {
    validFrames=0; psValid=false; return false;
  }
  previousButtons=buttons;
  buttons=uint16_t(~(uint16_t(rx[3])|(uint16_t(rx[4])<<8)));
  lastPS=millis(); psValid=true;
  if(validFrames<1000) validFrames++;
  return true;
}

enum State { LOCKED, UNCALIBRATED, DISARMED, READY, CHARGING, FORWARD, DWELL, RETURNING, FAULT };
void transition(State next); // 显式声明，避免Arduino自动原型出现在枚举定义之前。
State state=LOCKED;
bool prepared=false, aligned=false;
float home=0, targetArm=0, profileSpeed=0, selectedSpeed=MIN_SPEED;
uint32_t stateSince=0, chargeSince=0, lastControl=0, settledSince=0;
uint32_t lastMagnet=0, lastPrint=0, lastActivity=0;
bool magnetOK=false;

void transition(State next) {
  state=next; stateSince=millis(); settledSince=0;
}
void disableOutput() {
  if(prepared) motor.disable();
  digitalWrite(EN,LOW);
  profileSpeed=0;
}
void trip(const char *reason) {
  disableOutput(); transition(FAULT);
  Serial.print("FAULT: "); Serial.println(reason);
}
float armAngle() { return DIRECTION*(sensor.getAngle()-home)/MOTOR_TO_ARM_RATIO; }
bool movingState() { return state==FORWARD || state==DWELL || state==RETURNING; }
bool enabledState() { return state==READY || state==CHARGING || movingState(); }
bool psFresh() { return psValid && validFrames>=5 && millis()-lastPS<PS_TIMEOUT_MS; }

// 规划参考轨迹：加速、限速、按剩余距离制动，目标始终夹在0~90度。
// 实际角度由编码器闭环跟踪；负载/惯性过大会造成跟踪误差和超调。
void advanceProfile(float destination,float cap,float dt) {
  float remaining=destination-targetArm;
  float direction=remaining>=0?1.0f:-1.0f;
  float desired=direction*fminf(cap,sqrtf(2*ACCEL*fabsf(remaining)));
  float change=constrain(desired-profileSpeed,-ACCEL*dt,ACCEL*dt);
  profileSpeed+=change;
  float step=profileSpeed*dt;
  if(fabsf(step)>=fabsf(remaining)) { targetArm=destination; profileSpeed=0; }
  else targetArm=constrain(targetArm+step,0.0f,TRAVEL);
}
bool settled(float destination) {
  bool at=fabsf(armAngle()-destination)<POSITION_TOL &&
          fabsf(motor.shaft_velocity/MOTOR_TO_ARM_RATIO)<0.1f &&
          fabsf(targetArm-destination)<0.0001f;
  if(!at) { settledSince=0; return false; }
  if(!settledSince) settledSince=millis();
  return millis()-settledSince>=200;
}

// d：仅在电机禁用时检查I2C电平、尝试9时钟总线恢复并低速重读。
// 只操作用户已明确连接编码器的21/22，不扫描或驱动未知外设引脚。
// 电平只作诊断数据，不把高/低直接解释成“线路导通/断线”。
void diagnoseEncoder() {
  if(enabledState()) { Serial.println("d refused: disable motor first"); return; }
  disableOutput(); Wire.end();
  pinMode(SDA_PIN,INPUT); pinMode(SCL_PIN,INPUT); delay(5);
  Serial.printf("I2C floating: SDA=%d SCL=%d\n",digitalRead(SDA_PIN),digitalRead(SCL_PIN));
  pinMode(SDA_PIN,INPUT_PULLDOWN); pinMode(SCL_PIN,INPUT_PULLDOWN); delay(5);
  Serial.printf("I2C pulldown: SDA=%d SCL=%d\n",digitalRead(SDA_PIN),digitalRead(SCL_PIN));
  pinMode(SDA_PIN,INPUT_PULLUP); pinMode(SCL_PIN,INPUT_PULLUP); delay(5);
  Serial.printf("I2C pullup: SDA=%d SCL=%d\n",digitalRead(SDA_PIN),digitalRead(SCL_PIN));
  // 开漏时钟：释放高电平，避免主动向外设强推高电平。
  digitalWrite(SCL_PIN,HIGH); pinMode(SCL_PIN,OUTPUT_OPEN_DRAIN);
  uint8_t pulses=0;
  while(!digitalRead(SDA_PIN) && pulses<9) {
    digitalWrite(SCL_PIN,LOW); delayMicroseconds(100);
    digitalWrite(SCL_PIN,HIGH); delayMicroseconds(100); pulses++;
  }
  bool clockHigh=digitalRead(SCL_PIN);
  if(clockHigh) {
    digitalWrite(SCL_PIN,LOW);
    digitalWrite(SDA_PIN,LOW); pinMode(SDA_PIN,OUTPUT_OPEN_DRAIN); delayMicroseconds(100);
    digitalWrite(SCL_PIN,HIGH); delayMicroseconds(100);
    digitalWrite(SDA_PIN,HIGH); delayMicroseconds(100); // 产生STOP。
  }
  Serial.printf("I2C recovery: pulses=%u SCL_released=%d\n",pulses,int(clockHigh));
  pinMode(SDA_PIN,INPUT_PULLUP); pinMode(SCL_PIN,INPUT_PULLUP);
  Wire.begin(SDA_PIN,SCL_PIN); Wire.setTimeOut(3);
  const uint32_t rates[]={100000,50000,10000};
  for(uint32_t rate:rates) {
    Wire.setClock(rate); Wire.beginTransmission(0x36);
    uint8_t error=Wire.endTransmission();
    uint8_t status=0; bool statusOK=sensor.status(status); sensor.update();
    Serial.printf("I2C rate=%lu addr36_error=%u angleOK=%d statusOK=%d status=%02X deg=%.2f\n",
      (unsigned long)rate,error,int(sensor.ok),int(statusOK),status,sensor.cached*180/PI);
  }
  Wire.setClock(100000);
}

// 串口控制：x禁用；c卸杆校准；h人工放在机械原点后记录零点；a使能。
// 软件x与手柄圆圈只是辅助急停；独立断电急停必须另做。
void serialCommand(char cmd) {
  if(cmd=='x') { disableOutput(); transition(aligned?DISARMED:UNCALIBRATED); return; }
  if(cmd=='d') { diagnoseEncoder(); return; }
  if(!prepared) return;
  if(cmd=='c' && !enabledState()) {
    if(!sensor.ok || !magnetOK || !psFresh()) { Serial.println("Calibration refused: sensor/magnet/PS2"); return; }
    home=NAN; // 每次重新校准必须重新人工记录原点，禁止沿用旧零点。
    Serial.println("CALIBRATION: arm MUST be removed; blocking alignment may rotate motor.");
    motor.enable();
    aligned=(motor.initFOC()==1);
    disableOutput();
    transition(aligned?DISARMED:FAULT);
    // 校准会阻塞，期间软件急停无法保证响应；使用物理断电开关。
    return;
  }
  if(cmd=='h' && aligned && !enabledState() && sensor.ok && magnetOK) {
    home=sensor.getAngle(); targetArm=0; profileSpeed=0;
    transition(DISARMED);
    Serial.println("Home captured. Confirm physical arm rest position, then a.");
    return;
  }
  // a前必须显式h。NaN原点表示尚未记录；启动时setup会设置为NaN。
  if(cmd=='a' && state==DISARMED && aligned && isfinite(home) && psFresh() &&
     sensor.ok && magnetOK && buttons==0 && fabsf(armAngle())<POSITION_TOL) {
    motor.enable(); targetArm=0; profileSpeed=0; lastActivity=millis();
    transition(READY); Serial.println("ARMED: hold L1; hold CROSS to charge, release CROSS to move.");
  }
}

void setup() {
  pinMode(EN,OUTPUT); digitalWrite(EN,LOW);
  Serial.begin(115200);
  pinMode(PS_CS,OUTPUT); digitalWrite(PS_CS,HIGH);
  pinMode(PS_CLK,OUTPUT); digitalWrite(PS_CLK,HIGH);
  pinMode(PS_CMD,OUTPUT); digitalWrite(PS_CMD,HIGH);
  pinMode(PS_DAT,INPUT_PULLUP);
  Wire.begin(SDA_PIN,SCL_PIN); Wire.setClock(100000); Wire.setTimeOut(3);
  sensor.init(); home=NAN;
  // 未解锁时仍读编码器与手柄，便于先确认它们的连通性。
  if(!CONFIG_CONFIRMED || SUPPLY_VOLTAGE<8.2f || SUPPLY_VOLTAGE>16.0f ||
     MOTOR_TO_ARM_RATIO<=0 || fabsf(DIRECTION)!=1.0f) {
    Serial.println("LOCKED: motor disabled; PS2/AS5600 diagnostics only."); return;
  }
  driver.voltage_power_supply=SUPPLY_VOLTAGE;
  driver.voltage_limit=SUPPLY_VOLTAGE; driver.pwm_frequency=20000;
  if(!driver.init()) { Serial.println("Driver init failed"); return; }
  motor.linkSensor(&sensor); motor.linkDriver(&driver);
  motor.controller=MotionControlType::angle;
  motor.torque_controller=TorqueControlType::voltage;
  motor.voltage_limit=VOLTAGE_LIMIT; motor.voltage_sensor_align=VOLTAGE_LIMIT;
  motor.velocity_limit=MAX_SPEED*MOTOR_TO_ARM_RATIO;
  motor.P_angle.P=8.0f;
  motor.PID_velocity.P=0.2f; motor.PID_velocity.I=1.0f; motor.PID_velocity.D=0;
  motor.LPF_velocity.Tf=0.02f;
  motor.init(); prepared=true; disableOutput(); transition(UNCALIBRATED);
  Serial.println("c=calibrate unloaded, h=capture manual home, a=arm, x=disable");
}

void loop() {
  uint32_t now=millis();
  // 先更新编码器。启用时loopFOC会再次读取，之后还要检查该次读取是否成功。
  sensor.update();
  if(now-lastMagnet>=20) {
    lastMagnet=now; uint8_t status=0;
    magnetOK=sensor.status(status) && (status&0x20) && !(status&0x18);
  }
  if(enabledState() && (!sensor.ok || !magnetOK)) trip("encoder read/magnet");
  bool newFrame=false;
  if(now-lastPoll>=20) { lastPoll=now; newFrame=pollPS2(); }
  if(enabledState() && !psFresh()) trip("PS2 invalid/stale frames");
  if(newFrame && (buttons&CIRCLE)) trip("PS2 circle stop");
  while(Serial.available()) serialCommand(Serial.read());
  now=millis(); // 校准/串口处理可能耗时，必须刷新，避免无符号时间差回绕。

  if(enabledState()) {
    if((state==CHARGING || movingState()) && !(buttons&L1)) trip("L1 released");
    if(enabledState() && fabsf(motor.shaft_velocity/MOTOR_TO_ARM_RATIO)>MAX_SPEED*1.5f) trip("measured overspeed");
    if(enabledState() && (armAngle() < -RANGE_MARGIN || armAngle()>TRAVEL+RANGE_MARGIN)) trip("angle outside range");
    if(movingState() && now-stateSince>MOVE_TIMEOUT_MS) trip("movement timeout/stall");
    if((state==READY || state==CHARGING) && now-lastActivity>IDLE_TIMEOUT_MS) {
      disableOutput(); transition(DISARMED); Serial.println("Idle disarm");
    }
  }
  if(state==READY && newFrame && (buttons&L1) && (buttons&CROSS) && !(previousButtons&CROSS)) {
    chargeSince=now; lastActivity=now; transition(CHARGING);
  }
  if(state==CHARGING && newFrame && !(buttons&CROSS)) {
    uint32_t held=now-chargeSince;
    if(held<MIN_CHARGE_MS) transition(READY); // 消除误触。
    else {
      float fraction=fminf(1.0f,float(held)/FULL_CHARGE_MS);
      selectedSpeed=MIN_SPEED+(MAX_SPEED-MIN_SPEED)*fraction;
      profileSpeed=0; transition(FORWARD);
      Serial.print("Release: speed cap(rad/s)="); Serial.println(selectedSpeed);
    }
    lastActivity=now;
  }
  float dt=lastControl?fminf((micros()-lastControl)*1e-6f,0.01f):0;
  lastControl=micros();
  if(state==FORWARD) {
    advanceProfile(TRAVEL,selectedSpeed,dt);
    if(settled(TRAVEL)) transition(DWELL);
  } else if(state==DWELL && now-stateSince>=300) {
    profileSpeed=0; transition(RETURNING);
  } else if(state==RETURNING) {
    advanceProfile(0,RETURN_SPEED,dt);
    if(settled(0)) { lastActivity=now; transition(READY); }
  }
  if(enabledState()) {
    float cap=state==FORWARD?selectedSpeed:RETURN_SPEED;
    motor.updateVelocityLimit(cap*MOTOR_TO_ARM_RATIO);
    motor.loopFOC();
    if(!sensor.ok) trip("encoder error during FOC");
    else {
      motor.move(home+DIRECTION*targetArm*MOTOR_TO_ARM_RATIO);
      if(!sensor.ok) trip("encoder error during motion update");
    }
  }
  if(now-lastPrint>=500 && Serial.availableForWrite()>80) {
    lastPrint=now;
    Serial.printf("state=%d PS2=%d mode=%02X buttons=%04X enc=%d magnetOK=%d sensorDeg=%.1f arm=%.1f target=%.1f\n",
      int(state),int(psFresh()),psMode,buttons,int(sensor.ok),int(magnetOK),
      sensor.cached*180/PI,isfinite(home)?armAngle()*180/PI:0,targetArm*180/PI);
  }
}
