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
  assert(!jogging && !startupPulse && !remoteEnabled && simulatedPins[STBY]==0);
  for(int i=0;i<4;++i) {
    assert(wheels[i].value==0);
    assert(!simulatedDuty[PINS[i][0]] && !simulatedDuty[PINS[i][1]]);
  }
}
void selectedOutputs(int index,int logical) {
  assert(simulatedPins[STBY]==1 && startupPulse && jogging);
  for(int i=0;i<4;++i) {
    int expected=i==index?logical*POLARITY[i]:0;
    assert(wheels[i].value==expected);
    assert(simulatedDuty[PINS[i][0]]==(expected>0?expected:0));
    assert(simulatedDuty[PINS[i][1]]==(expected<0?-expected:0));
  }
}
void arm() {
  tickAt(millis()+500);
  assert(send("STARTPULSE RR 192 2000").find("OK STARTPULSE")!=std::string::npos);
  tickAt(millis()+10); selectedOutputs(3,255);
}
int main() {
  setup(); healthy(); idleOutputs();
  assert(send("STARTPULSEINFO").find("kick300-v1 startDuty=255 startMs=300 maxDuty=255 minMs=500 maxMs=2000")!=std::string::npos);
  const char *bad[]={"STARTPULSE XX 192 2000","STARTPULSE RR 0 2000",
    "STARTPULSE RR 256 2000","STARTPULSE RR -256 2000","STARTPULSE RR -2147483648 2000",
    "STARTPULSE RR 192 499","STARTPULSE RR 192 2001","STARTPULSE RR 192",
    "STARTPULSE RR 192 2000 extra"};
  for(const char *request:bad) { assert(send(request).find("ERR STARTPULSE")!=std::string::npos); idleOutputs(); }
  // Actual GPIO writes: all labels, both polarities, exact stage boundary,
  // total expiry, no inactive wheel, and a timer crossing UINT32_MAX.
  for(int wheel=0;wheel<4;++wheel) for(int sign:{1,-1}) {
    tickAt(millis()+500); send("COMP ON");
    uint32_t start=millis(); char request[80];
    snprintf(request,sizeof(request),"STARTPULSE %s %d 2000",LABELS[wheel],sign*192);
    assert(send(request).find("OK STARTPULSE")!=std::string::npos);
    for(uint32_t t=0;t<2000;t+=10) { tickAt(start+t); selectedOutputs(wheel,sign*(t<300?255:192)); }
    tickAt(start+2000); idleOutputs(); assert(!allWheelsQuiet());
    assert(send(request).find("ERR STARTPULSE")!=std::string::npos); idleOutputs();
  }
  simulatedMillis=0xffffff00u; healthy();
  for(auto &wheel:wheels) wheel.zeroAt=simulatedMillis-500;
  uint32_t start=millis(); send("STARTPULSE RR 192 2000");
  tickAt(start+299); selectedOutputs(3,255);
  tickAt(start+300); selectedOutputs(3,192);
  tickAt(start+2000); idleOutputs();
  assert(send("STARTMOVEINFO").find("kick300-fwd-v1 startDuty=255 startMs=300 maxDuty=192 minMs=500 maxMs=30000")!=std::string::npos);
  for(const char *request:{"STARTMOVE BACK 192 2000","STARTMOVE FWD 193 2000",
                          "STARTMOVE FWD 0 2000","STARTMOVE FWD -192 2000",
                          "STARTMOVE FWD 192 499","STARTMOVE FWD 192 30001",
                          "STARTMOVE FWD 192 2000 extra"}) {
    assert(send(request).find("ERR STARTMOVE")!=std::string::npos); idleOutputs();
  }
  // Four-wheel startup preserves calibrated signs, explicit trim mode,
  // 300ms phase boundary and total 30s expiry.
  for(bool trimmed:{false,true}) {
    tickAt(millis()+500); send(trimmed?"COMP ON":"COMP OFF");
    start=millis(); assert(send("STARTMOVE FWD 192 30000").find("OK STARTMOVE")!=std::string::npos);
    for(uint32_t t=0;t<30000;t+=10) {
      tickAt(start+t); assert(startupPulse && motionPulse && simulatedPins[STBY]==1);
      for(int i=0;i<4;++i) {
        int logical=t<300?255:(trimmed?trimmedDuty(i,192):192);
        int expected=logical*POLARITY[i];
        assert(wheels[i].value==expected);
        assert(simulatedDuty[PINS[i][0]]==(expected>0?expected:0));
        assert(simulatedDuty[PINS[i][1]]==(expected<0?-expected:0));
      }
    }
    tickAt(start+30000); idleOutputs();
    assert(send("STARTMOVE FWD 192 2000").find("ERR STARTMOVE")!=std::string::npos); idleOutputs();
  }
  for(uint32_t stop_at:{100u,1000u}) {
    tickAt(millis()+500); start=millis(); send("STARTMOVE FWD 192 2000");
    tickAt(start+stop_at); send("STOP"); idleOutputs();
  }
  tickAt(millis()+500); send("STARTMOVE FWD 192 2000"); tickAt(millis()+10);
  assert(send("PULSE RR 192 2000").find("ERR PULSE")!=std::string::npos); idleOutputs();
  // STOP, overlap, button/link/hardware failures always remove both stages.
  arm(); send("STOP"); idleOutputs();
  arm(); assert(send("STARTPULSE FL 192 2000").find("ERR STARTPULSE")!=std::string::npos); idleOutputs();
  arm(); assert(send("PULSE RR 192 2000").find("ERR PULSE")!=std::string::npos); idleOutputs();
  arm(); psValid=false; tickDrive(); idleOutputs();
  arm(); buttons=UP; tickDrive(); idleOutputs();
  arm(); hardwareOK=false; tickDrive(); idleOutputs(); hardwareOK=true;
  tickAt(millis()+500); send("PS2");
  assert(send("STARTPULSE RR 192 2000").find("ERR STARTPULSE")!=std::string::npos); idleOutputs();
  // A later raw pulse must not inherit startup PWM.
  arm(); send("STOP"); tickAt(millis()+500);
  send("PULSE RR 192 2000"); tickAt(millis()+10);
  assert(!startupPulse && wheels[3].value==-192); send("STOP"); idleOutputs();
  puts("PASS: startup actual GPIO stages, labels/polarities, expiry/wraparound, bounds, STOP and interlocks; raw pulse preserved");
  puts("PASS: explicit four-wheel FWD startup, 30s expiry, raw/trimmed hold, STOP and preserved raw limits");
}
