// Classic ESP32 / Arduino ESP32 3.x. TB6612 PWMA/PWMB fixed at 3.3V.
// AS5600 intentionally unused. Catapult EN and phase inputs remain low.
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_task_wdt.h>
#include <string.h>
#include "DriveMath.h"

constexpr uint8_t STBY=33, PS_CLK=2, PS_CS=4, PS_CMD=12, PS_DAT=13;
constexpr uint8_t PINS[4][2]={{16,17},{18,19},{23,32},{15,5}};
const char *const LABELS[4]={"FL","RL","FR","RR"};
// Calibrate each wheel on stands before driving on the ground.
constexpr int POLARITY[4]={1,1,-1,-1}; // Verified on the assembled chassis.
constexpr int CAP=76; // 29.8% duty ceiling; NOT a current or speed limit.
constexpr int JOG_CAP=128; // Preserve original JOG ceiling.
constexpr int ALL_PULSE_CAP=192; // 75.3%, bench diagnostic only; not a current limit.
constexpr int ALL_PULSE_MAX_MS=300; // Software timeout, not independent hardware timing.
constexpr int MOTION_PULSE_MAX_MS=10000; // Bench observation, software timeout.
constexpr uint16_t UP=1<<4, RIGHT=1<<5, DOWN=1<<6, LEFT=1<<7;
constexpr uint16_t L2=1<<8, R2=1<<9, L1=1<<10, CIRCLE=1<<13;
constexpr uint16_t START=1<<3;
bool attached[4][2]={}, hardwareOK=false, psValid=false, neutralSeen=false;
uint8_t mode=0;
uint16_t buttons=0, goodFrames=0;
uint32_t lastPS=0, lastPoll=0, lastTick=0, lastPrint=0, jogAt=0;
bool remoteEnabled=false, jogging=false;
// Fixed duty: selected <=500ms, ALL <=300ms, named motion <=10s; software timeouts.
bool diagnosticPulse=false, allPulse=false, motionPulse=false;
struct Motion { const char *name; int y, x, r; };
constexpr Motion MOTIONS[]={
  {"FWD",1,0,0},{"BACK",-1,0,0},{"LEFT",0,-1,0},{"RIGHT",0,1,0},
  {"FL",1,-1,0},{"FR",1,1,0},{"BL",-1,-1,0},{"BR",-1,1,0},
  {"CCW",0,0,-1},{"CW",0,0,1}
};
int motionTarget[4]={};
int jogWheel=0, jogDuty=0, jogMs=0;
int target[4]={};
WheelRamp wheels[4];
char line[80]; size_t used=0; bool overflow=false;
esp_task_wdt_user_handle_t watchdog=nullptr;

void lowPin(uint8_t pin) {
  gpio_set_level(static_cast<gpio_num_t>(pin),0); pinMode(pin,OUTPUT);
}
void stopDrive() {
  digitalWrite(STBY,LOW);
  for (int i=0;i<4;++i) {
    for (int j=0;j<2;++j) if(attached[i][j]) ledcWrite(PINS[i][j],0);
    target[i]=0; wheels[i].stop(millis());
  }
}
void lockDrive() {
  stopDrive(); remoteEnabled=false; jogging=false; diagnosticPulse=false; allPulse=false; motionPulse=false; neutralSeen=false;
}
bool allWheelsQuiet() {
  uint32_t now=millis();
  for(int i=0;i<4;++i) if(wheels[i].value || uint32_t(now-wheels[i].zeroAt)<300) return false;
  return true;
}
void printInfo() {
  Serial.printf("INFO firmware=MecanumPS2-v1.1+diag-motion polarity=%d,%d,%d,%d allCap=%d allMaxMs=%d\n",
    POLARITY[0],POLARITY[1],POLARITY[2],POLARITY[3],ALL_PULSE_CAP,ALL_PULSE_MAX_MS);
}
bool freshPS() { return psValid && goodFrames>=5 && uint32_t(millis()-lastPS)<150; }
uint8_t exchangeByte(uint8_t tx) {
  uint8_t rx=0;
  for(uint8_t bit=0;bit<8;++bit) {
    digitalWrite(PS_CMD,(tx>>bit)&1);
    digitalWrite(PS_CLK,LOW); delayMicroseconds(4);
    if(digitalRead(PS_DAT)) rx|=1<<bit;
    digitalWrite(PS_CLK,HIGH); delayMicroseconds(4);
  }
  digitalWrite(PS_CMD,HIGH); delayMicroseconds(20); return rx;
}
void pollPS() {
  uint8_t rx[21]={}; uint8_t length=9;
  digitalWrite(PS_CS,LOW); delayMicroseconds(20);
  for(uint8_t i=0;i<length;++i) {
    rx[i]=exchangeByte(i==0?1:(i==1?0x42:0));
    if(i==1 && rx[1]==0x79) length=21;
  }
  digitalWrite(PS_CS,HIGH); mode=rx[1];
  psValid=rx[2]==0x5A && (mode==0x41 || mode==0x73 || mode==0x79);
  if(!psValid) { goodFrames=0; lockDrive(); return; }
  uint16_t previous=buttons;
  buttons=uint16_t(~(uint16_t(rx[3]) | uint16_t(rx[4])<<8));
  lastPS=millis(); if(goodFrames<1000) ++goodFrames;
  if(buttons&CIRCLE) { lockDrive(); return; }
  if(freshPS() && buttons==0) neutralSeen=true;
  if(hardwareOK && freshPS() && neutralSeen && buttons==START && !(previous&START) && !jogging) {
    stopDrive(); remoteEnabled=true;
    Serial.println("OK PS2 START unlocked; hold L1 + direction");
  }
}
void printStatus() {
  Serial.printf("STATUS hw=%d ps=%d mode=%02X buttons=%04X control=%s STBY=%d duty=%d,%d,%d,%d\n",
    int(hardwareOK),int(freshPS()),mode,buttons,
    jogging?(motionPulse?"MOVEPULSE":(allPulse?"ALLPULSE":(diagnosticPulse?"PULSE":"JOG"))):(remoteEnabled?"PS2":"LOCKED"),digitalRead(STBY),
    wheels[0].value,wheels[1].value,wheels[2].value,wheels[3].value);
}
void command() {
  line[used]=0;
  if(overflow) { lockDrive(); Serial.println("ERR overflow; LOCKED"); }
  else if(!used) { }
  else if(!strcmp(line,"STOP")) { lockDrive(); Serial.println("OK STOP LOCKED"); }
  else if(!strcmp(line,"STATUS")) printStatus();
  else if(!strcmp(line,"INFO")) printInfo();
  else if(!strcmp(line,"MOTIONINFO")) Serial.printf("MOTIONINFO maxDuty=%d maxMs=%d\n",ALL_PULSE_CAP,MOTION_PULSE_MAX_MS);
  else if(!strcmp(line,"PS2")) {
    if(hardwareOK && freshPS() && buttons==0 && !jogging) {
      stopDrive(); remoteEnabled=true; neutralSeen=true;
      Serial.println("OK PS2: hold L1 + D-pad / L2 / R2; CIRCLE locks");
    } else { lockDrive(); Serial.println("ERR PS2 requires healthy link, released buttons, idle motors"); }
  } else if(!strncmp(line,"MOVEPULSE ",10)) {
    char name[6]={}, extra=0; int duty=0, duration=0;
    int n=sscanf(line,"MOVEPULSE %5s %d %d %c",name,&duty,&duration,&extra);
    int index=-1;
    for(size_t i=0;i<sizeof(MOTIONS)/sizeof(MOTIONS[0]);++i)
      if(!strcmp(name,MOTIONS[i].name)) index=int(i);
    if(n==3 && index>=0 && duty>=1 && duty<=ALL_PULSE_CAP &&
       duration>=50 && duration<=MOTION_PULSE_MAX_MS && hardwareOK && freshPS() && buttons==0 &&
       !remoteEnabled && !jogging && allWheelsQuiet()) {
      stopDrive();
      mixMecanum(MOTIONS[index].y,MOTIONS[index].x,MOTIONS[index].r,duty,motionTarget);
      jogMs=duration; jogAt=millis();
      diagnosticPulse=true; allPulse=false; motionPulse=true; jogging=true;
      Serial.printf("OK MOVEPULSE %s duty=%d duration=%dms fixedDuty=noRamp\n",name,duty,duration);
    } else { lockDrive(); Serial.println("ERR MOVEPULSE/interlock; LOCKED"); }
  } else if(!strncmp(line,"ALLPULSE ",9)) {
    char extra=0; int duty=0, duration=0;
    int n=sscanf(line,"ALLPULSE %d %d %c",&duty,&duration,&extra);
    if(n==2 && duty>=-ALL_PULSE_CAP && duty<=ALL_PULSE_CAP && duty!=0 &&
       duration>=50 && duration<=ALL_PULSE_MAX_MS && hardwareOK && freshPS() && buttons==0 &&
       !remoteEnabled && !jogging && allWheelsQuiet()) {
      stopDrive(); jogWheel=0; jogDuty=duty; jogMs=duration; jogAt=millis();
      diagnosticPulse=true; allPulse=true; motionPulse=false; jogging=true;
      Serial.printf("OK ALLPULSE duty=%d duration=%dms fixedDuty=noRamp\n",duty,duration);
    } else { lockDrive(); Serial.println("ERR ALLPULSE/interlock; LOCKED"); }
  } else if(!strncmp(line,"PULSE ",6)) {
    char wheel[3]={}, extra=0; int duty=0, duration=0;
    int n=sscanf(line,"PULSE %2s %d %d %c",wheel,&duty,&duration,&extra);
    // Use explicit signed bounds, including rejection of INT_MIN.
    int index=-1; for(int i=0;i<4;++i) if(!strcmp(wheel,LABELS[i])) index=i;
    if(n==3 && index>=0 && duty>=-255 && duty<=255 && duty!=0 &&
       duration>=50 && duration<=500 && hardwareOK && freshPS() && buttons==0 &&
       !remoteEnabled && !jogging && uint32_t(millis()-wheels[index].zeroAt)>=300) {
      stopDrive(); jogWheel=index; jogDuty=duty; jogMs=duration; jogAt=millis();
      diagnosticPulse=true; allPulse=false; motionPulse=false; jogging=true;
      Serial.printf("OK PULSE %s duty=%d duration=%dms fixedDuty=noRamp\n",wheel,duty,duration);
    } else { lockDrive(); Serial.println("ERR PULSE/interlock; LOCKED"); }
  } else {
    char wheel[3]={}, extra=0; int duty=0, duration=0;
    int n=sscanf(line,"JOG %2s %d %d %c",wheel,&duty,&duration,&extra);
    int index=-1; for(int i=0;i<4;++i) if(!strcmp(wheel,LABELS[i])) index=i;
    if(n==3 && index>=0 && duty && magnitude(duty)<=JOG_CAP && duration>=50 && duration<=800 &&
       hardwareOK && freshPS() && buttons==0 && !remoteEnabled && !jogging) {
      stopDrive(); diagnosticPulse=false; allPulse=false; motionPulse=false; jogWheel=index; jogDuty=duty; jogMs=duration; jogAt=millis(); jogging=true;
      Serial.printf("OK JOG %s duty=%d duration=%dms\n",wheel,duty,duration);
    } else { lockDrive(); Serial.println("ERR command/interlock; LOCKED"); }
  }
  used=0; overflow=false;
}
void tickDrive() {
  if(!hardwareOK || !freshPS() || (buttons&CIRCLE)) { lockDrive(); return; }
  for(int i=0;i<4;++i) target[i]=0;
  if(jogging) {
    if(uint32_t(millis()-jogAt)>=uint32_t(jogMs) || buttons) {
      lockDrive(); Serial.println("JOG DONE/STOP; LOCKED"); return;
    }
    if(motionPulse) for(int i=0;i<4;++i) target[i]=motionTarget[i];
    else if(allPulse) for(int i=0;i<4;++i) target[i]=jogDuty;
    else target[jogWheel]=jogDuty;
  } else if(remoteEnabled && neutralSeen && (buttons&L1)) {
    int y=int(bool(buttons&UP))-int(bool(buttons&DOWN));
    int x=int(bool(buttons&RIGHT))-int(bool(buttons&LEFT));
    int r=int(bool(buttons&R2))-int(bool(buttons&L2));
    mixMecanum(y,x,r,CAP,target);
  } else { stopDrive(); return; }
  bool moving=false, ok=true;
  for(int i=0;i<4;++i) {
    int value;
    if(diagnosticPulse) {
      value=target[i]*POLARITY[i];
      wheels[i].value=value;
      if(value) wheels[i].lastSign=value>0?1:-1;
    } else value=wheels[i].step(target[i]*POLARITY[i],millis());
    moving|=value!=0;
    // First zero the inactive direction, then write the active direction.
    if(value>=0) { ok=ledcWrite(PINS[i][1],0)&&ok; ok=ledcWrite(PINS[i][0],value)&&ok; }
    else { ok=ledcWrite(PINS[i][0],0)&&ok; ok=ledcWrite(PINS[i][1],-value)&&ok; }
  }
  if(!ok) { hardwareOK=false; lockDrive(); Serial.println("FAULT PWM; LOCKED"); return; }
  digitalWrite(STBY,moving?HIGH:LOW);
}
void setup() {
  lowPin(STBY); lowPin(14); lowPin(25); lowPin(26); lowPin(27);
  Serial.begin(115200);
  hardwareOK=true;
  for(int i=0;i<4;++i) for(int j=0;j<2;++j) {
    lowPin(PINS[i][j]);
    attached[i][j]=ledcAttachChannel(PINS[i][j],20000,8,i*2+j);
    if(!attached[i][j] || !ledcWrite(PINS[i][j],0)) hardwareOK=false;
  }
  pinMode(PS_CS,OUTPUT); digitalWrite(PS_CS,HIGH);
  pinMode(PS_CLK,OUTPUT); digitalWrite(PS_CLK,HIGH);
  pinMode(PS_CMD,OUTPUT); digitalWrite(PS_CMD,HIGH); pinMode(PS_DAT,INPUT_PULLUP);
  lockDrive();
  esp_task_wdt_config_t config={1000,0,true};
  esp_err_t result=esp_task_wdt_init(&config);
  if(result==ESP_ERR_INVALID_STATE) result=esp_task_wdt_reconfigure(&config);
  if(result!=ESP_OK || esp_task_wdt_add_user("mecanum-loop",&watchdog)!=ESP_OK) hardwareOK=false;
  Serial.println("BOOT MecanumPS2 v1.1+diag-motion LOCKED; AS5600 unused; catapult disabled");
  Serial.println("STATUS | STOP | PS2 | JOG FL/RL/FR/RR signedDuty(1..128) ms(50..800)");
  Serial.println("DIAGNOSTIC: PULSE FL/RL/FR/RR signedDuty(1..255) ms(50..500); fixed duty, no ramp; single wheel only");
  Serial.println("DIAGNOSTIC: ALLPULSE signedDuty(1..192) ms(50..300); bench only");
  Serial.println("DIAGNOSTIC: MOVEPULSE FWD/BACK/LEFT/RIGHT/FL/FR/BL/BR/CCW/CW duty(1..192) ms(50..10000); bench only");
  printInfo();
  printStatus();
}
void loop() {
  if(millis()-lastPoll>=20) { lastPoll=millis(); pollPS(); }
  if(!freshPS()) lockDrive();
  for(uint8_t n=0;n<64 && Serial.available();++n) {
    char ch=char(Serial.read());
    if(ch=='\n' || ch=='\r') command();
    else if(!overflow) {
      if(used<sizeof(line)-1 && ch>=32 && ch<=126) line[used++]=ch;
      else { overflow=true; lockDrive(); }
    }
  }
  if(millis()-lastTick>=10) { lastTick=millis(); tickDrive(); }
  if(millis()-lastPrint>=500 && Serial.availableForWrite()>120) { lastPrint=millis(); printStatus(); }
  if(watchdog) esp_task_wdt_reset_user(watchdog);
  delay(1);
}

