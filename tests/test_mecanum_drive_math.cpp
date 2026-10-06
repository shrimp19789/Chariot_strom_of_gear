#include "../firmware/chassis/MecanumPS2/DriveMath.h"
#include <assert.h>
#include <stdio.h>
int main() {
  int out[4];
  mixMecanum(1,0,0,76,out); for(int v:out) assert(v==76);
  mixMecanum(0,1,0,76,out); assert(out[0]==76 && out[1]==-76 && out[2]==-76 && out[3]==76);
  mixMecanum(0,0,1,76,out); assert(out[0]==76 && out[1]==76 && out[2]==-76 && out[3]==-76);
  for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) for(int r=-1;r<=1;++r) {
    mixMecanum(y,x,r,76,out); for(int v:out) assert(magnitude(v)<=76);
  }
  mixMecanum(0,0,0,76,out); for(int v:out) assert(v==0);
  WheelRamp w;
  for(uint32_t t=0;t<300;t+=10) w.step(76,t);
  assert(w.value==76);
  uint32_t t=300;
  while(w.value) { assert(w.step(-76,t)>=0); t+=10; }
  uint32_t zero=w.zeroAt;
  for(t=zero+10;t<zero+300;t+=10) assert(w.step(-76,t)==0);
  assert(w.step(-76,zero+300)<0);
  w.stop(zero+310);
  assert(w.step(76,zero+320)==0); // STOP cannot bypass reversal delay.
  assert(w.step(76,zero+610)>0);
  WheelRamp wrap; wrap.value=20; wrap.lastSign=1; wrap.stop(0xfffffff0u);
  assert(wrap.step(-20,20)==0);
  assert(wrap.step(-20,300)<0);
  puts("PASS: wheel mixing, duty bounds, zero input, reversal coast, STOP history, timer wrap");
}
