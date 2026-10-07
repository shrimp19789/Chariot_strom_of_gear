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
void idleOutputs() {
  assert(!jogging && !rampPulse && !remoteEnabled && simulatedPins[STBY]==0);
  for(int i=0;i<4;++i) {
    assert(wheels[i].value==0);
    for(int j=0;j<2;++j) assert(simulatedDuty[PINS[i][j]]==0);
  }
}
void tickAt(uint32_t now) { simulatedMillis=now; healthy(); tickDrive(); }

int main() {
  setup(); healthy(); idleOutputs();
  assert(send("RAMPINFO").find("maxDuty=192 minMs=1000 maxMs=10000")!=std::string::npos);
  const char *bad[]={"RAMPPULSE LEFT 192 10000","RAMPPULSE FWD 193 10000",
    "RAMPPULSE FWD -192 10000","RAMPPULSE FWD 0 10000",
    "RAMPPULSE FWD 192 999","RAMPPULSE FWD 192 10001",
    "RAMPPULSE FWD 192 10000 extra","RAMPPULSE FWD 192"};
  for(const char *text:bad) { assert(send(text).find("ERR RAMPPULSE")!=std::string::npos); idleOutputs(); }
  for(int direction:{1,-1}) {
    uint32_t start=millis()+500; tickAt(start);
    const char *cmd=direction==1?"RAMPPULSE FWD 192 10000":"RAMPPULSE BACK 192 10000";
    assert(send(cmd).find("OK RAMPPULSE")!=std::string::npos);
    for(uint32_t elapsed=0;elapsed<10000;elapsed+=10) {
      tickAt(start+elapsed);
      assert(jogging && rampPulse);
      int duty=triangleDuty(192,elapsed,10000);
      assert(simulatedPins[STBY]==int(duty!=0));
      for(int i=0;i<4;++i) {
        int expected=direction*POLARITY[i]*duty;
        assert(wheels[i].value==expected);
        assert(simulatedDuty[PINS[i][0]]==(expected>0?expected:0));
        assert(simulatedDuty[PINS[i][1]]==(expected<0?-expected:0));
      }
    }
    tickAt(start+10000); idleOutputs(); assert(!allWheelsQuiet());
    assert(send(cmd).find("ERR RAMPPULSE")!=std::string::npos); idleOutputs();
  }
  tickAt(millis()+500); assert(send("RAMPPULSE FWD 192 10000").find("OK RAMPPULSE")!=std::string::npos);
  tickAt(millis()+500); assert(send("STOP").find("OK STOP")!=std::string::npos); idleOutputs();
  assert(send("RAMPPULSE BACK 192 10000").find("ERR RAMPPULSE")!=std::string::npos);
  tickAt(millis()+500); send("RAMPPULSE FWD 192 10000"); tickAt(millis()+100);
  assert(send("MOVEPULSE LEFT 192 10000").find("ERR MOVEPULSE")!=std::string::npos); idleOutputs();
  tickAt(millis()+500); send("MOVEPULSE RIGHT 192 10000"); tickAt(millis()+100);
  assert(send("RAMPPULSE FWD 192 10000").find("ERR RAMPPULSE")!=std::string::npos); idleOutputs();
  tickAt(millis()+500); send("PS2");
  assert(send("RAMPPULSE FWD 192 10000").find("ERR RAMPPULSE")!=std::string::npos); idleOutputs();
  tickAt(millis()+500); send("RAMPPULSE FWD 192 10000"); tickAt(millis()+100);
  psValid=false; tickDrive(); idleOutputs();
  tickAt(millis()+500); assert(send("RAMPPULSE FWD 192 10000").find("OK RAMPPULSE")!=std::string::npos); tickAt(millis()+100);
  buttons=UP; tickDrive(); idleOutputs();
  // Existing lateral commands retain their calibrated output patterns.
  tickAt(millis()+500); assert(send("MOVEPULSE LEFT 192 10000").find("OK MOVEPULSE")!=std::string::npos); tickAt(millis()+100);
  const int left[]={-192,192,-192,192};
  for(int i=0;i<4;++i) assert(wheels[i].value==left[i]);
  send("STOP"); tickAt(millis()+500); send("MOVEPULSE RIGHT 192 10000"); tickAt(millis()+100);
  for(int i=0;i<4;++i) assert(wheels[i].value==-left[i]);
  send("STOP"); idleOutputs();
  puts("PASS: actual firmware ramp PWM, expiry, bounds, STOP/cooldown, overlap, PS2/button guards and lateral signs");
}
