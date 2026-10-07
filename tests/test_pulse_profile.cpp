#include "../firmware/chassis/MecanumPS2MotionDiagnostic/PulseProfile.h"
#include <assert.h>
#include <stdio.h>

int main() {
  // Exercise duration boundaries and every peak, checking sign, bounds,
  // monotonicity, exact midpoint and software expiry (including odd durations).
  const uint32_t durations[]={1000,1001,1999,2000,9999,10000};
  for (uint32_t duration:durations) {
    for (int peak=1; peak<=192; ++peak) {
      int previous=0;
      for (uint32_t t=0; t<=duration; ++t) {
        int value=triangleDuty(peak,t,duration);
        assert(value>=0 && value<=peak);
        assert(triangleDuty(-peak,t,duration)==-value);
        if(t<=duration/2) assert(value>=previous);
        else assert(value<=previous);
        previous=value;
      }
      assert(triangleDuty(peak,0,duration)==0);
      assert(triangleDuty(peak,duration/2,duration)==peak);
      assert(triangleDuty(peak,duration,duration)==0);
      assert(triangleDuty(peak,0xffffffffu,duration)==0);
    }
  }
  assert(triangleDuty(192,0,0)==0);
  assert(triangleDuty(192,0,1)==0);
  // Unsigned elapsed-time subtraction across a millis() wrap.
  uint32_t start=0xffffff00u, now=start+1000u;
  assert(triangleDuty(192,now-start,2000)==192);
  puts("PASS: triangular duty bounds, directions, slopes, midpoint, expiry and timer wrap");
}
