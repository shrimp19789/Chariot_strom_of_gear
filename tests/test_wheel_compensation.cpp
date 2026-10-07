#include <Arduino.h>
#include <assert.h>
#include <string.h>
uint32_t simulatedMillis=1000;
int simulatedPins[40]={},simulatedDuty[40]={};
SimulatedSerial Serial;
#include "../firmware/chassis/MecanumPS2MotionDiagnostic/MecanumPS2MotionDiagnostic.ino"

void healthy() { psValid=true; goodFrames=10; lastPS=millis(); buttons=0; }
std::string send(const char *text) {
  assert(strlen(text)<sizeof(line)); strcpy(line,text); used=strlen(text); overflow=false;
  Serial.output.clear(); command(); return Serial.output;
}
void tickAt(uint32_t now) { simulatedMillis=now; healthy(); tickDrive(); }
void idleOutputs() {
  assert(!jogging && !remoteEnabled && simulatedPins[STBY]==0);
  for(int i=0;i<4;++i) {
    assert(wheels[i].value==0);
    assert(!simulatedDuty[PINS[i][0]] && !simulatedDuty[PINS[i][1]]);
  }
}
void expectOutputs(const int logical[4]) {
  for(int i=0;i<4;++i) {
    const int expected=logical[i]*POLARITY[i];
    assert(wheels[i].value==expected);
    assert(simulatedDuty[PINS[i][0]]==(expected>0?expected:0));
    assert(simulatedDuty[PINS[i][1]]==(expected<0?-expected:0));
  }
}
int main() {
  setup(); healthy(); idleOutputs(); assert(!compensationEnabled);
  const int full[]={123,108,144,192};
  for(int i=0;i<4;++i) {
    assert(trimmedDuty(i,192)==full[i]); assert(trimmedDuty(i,-192)==-full[i]);
    int last=0;
    for(int raw=0;raw<=192;++raw) {
      int value=trimmedDuty(i,raw);
      assert(value>=last && value<=192 && trimmedDuty(i,-raw)==-value);
      last=value;
    }
    assert(trimmedDuty(i,0)==0 && trimmedDuty(i,255)==full[i]);
    assert(trimmedDuty(i,-2147483647-1)==-full[i]);
  }
  assert(send("COMP ON extra").find("ERR COMP")!=std::string::npos); idleOutputs();
  assert(send("COMP ON").find("OK COMP ON")!=std::string::npos);
  assert(send("COMPINFO").find("enabled=1 gain=76/119,76/135,76/101,1/1 maxDuty=192")!=std::string::npos);
  // Every movement preserves its wheel pattern and inactive wheels at zero.
  for(const auto &motion:MOTIONS) {
    tickAt(millis()+500);
    char request[80]; snprintf(request,sizeof(request),"MOVEPULSE %s 192 300",motion.name);
    assert(send(request).find("OK MOVEPULSE")!=std::string::npos);
    int raw[4], expected[4]; mixMecanum(motion.y,motion.x,motion.r,192,raw);
    for(int i=0;i<4;++i) expected[i]=raw[i]==0?0:(raw[i]>0?full[i]:-full[i]);
    uint32_t start=millis(); tickAt(start+100); expectOutputs(expected);
    tickAt(start+300); idleOutputs(); assert(!allWheelsQuiet());
  }
  // Long forward pulse keeps the same independent duties and expires at 30s.
  tickAt(millis()+500); uint32_t start=millis();
  assert(send("MOVEPULSE FWD 192 30000").find("OK MOVEPULSE")!=std::string::npos);
  for(uint32_t t=10;t<30000;t+=10) { tickAt(start+t); expectOutputs(full); }
  tickAt(start+30000); idleOutputs();
  // Enable/disable is forbidden during a motion and stops all outputs.
  tickAt(millis()+500); send("MOVEPULSE FWD 192 300"); tickAt(millis()+10);
  assert(send("COMP OFF").find("ERR COMP")!=std::string::npos); idleOutputs();
  assert(compensationEnabled);
  assert(send("COMP OFF").find("ERR COMP")!=std::string::npos); // Cooldown.
  tickAt(millis()+500); assert(send("COMP OFF").find("OK COMP OFF")!=std::string::npos);
  send("COMP ON");
  // Single-wheel, ALL and JOG remain raw even when compensation is on.
  for(int i=0;i<4;++i) {
    tickAt(millis()+500); char request[60];
    snprintf(request,sizeof(request),"PULSE %s 192 300",LABELS[i]); send(request);
    tickAt(millis()+100); int expected[4]={}; expected[i]=192; expectOutputs(expected);
    send("STOP"); idleOutputs();
  }
  tickAt(millis()+500); send("ALLPULSE 192 300"); tickAt(millis()+100);
  const int rawFull[]={192,192,192,192}; expectOutputs(rawFull); send("STOP");
  tickAt(millis()+500); send("JOG FR 128 800");
  for(int t=0;t<50;++t) tickAt(millis()+10);
  const int rawJog[]={0,0,128,0}; expectOutputs(rawJog); send("STOP");
  // Ramped outputs use the same gains without early saturation or sign errors.
  for(int direction=1;direction>=-1;direction-=2) {
    tickAt(millis()+500); start=millis();
    send(direction>0?"RAMPPULSE FWD 192 10000":"RAMPPULSE BACK 192 10000");
    for(uint32_t t=0;t<10000;t+=10) {
      tickAt(start+t); int base=triangleDuty(192,t,10000),expected[4];
      for(int i=0;i<4;++i) expected[i]=direction*trimmedDuty(i,base);
      expectOutputs(expected);
    }
    tickAt(start+10000); idleOutputs();
  }
  // PS2 scaling also occurs before the original ramp and direction guard.
  tickAt(millis()+500); send("PS2");
  for(int t=0;t<40;++t) { simulatedMillis+=10; healthy(); buttons=L1|UP; tickDrive(); }
  const int ps2Full[]={49,43,57,76}; expectOutputs(ps2Full);
  healthy(); buttons=CIRCLE; tickDrive(); idleOutputs();
  tickAt(millis()+500); send("MOVEPULSE FWD 192 300"); tickAt(millis()+10);
  psValid=false; tickDrive(); idleOutputs();
  tickAt(millis()+500); send("MOVEPULSE FWD 192 300"); tickAt(millis()+10);
  buttons=UP; tickDrive(); idleOutputs();
  puts("PASS: compensated actual firmware PWM, all wheel patterns, raw diagnostics, PS2, ramp, 30s expiry, STOP and guards");
}
