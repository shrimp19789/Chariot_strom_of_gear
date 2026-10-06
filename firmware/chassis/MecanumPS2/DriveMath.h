#pragma once
#include <stdint.h>

// Order: FL, RL, FR, RR. Positive y=forward, x=right, r=clockwise.
inline int magnitude(int n) { return n < 0 ? -n : n; }
inline void mixMecanum(int y, int x, int r, int cap, int out[4]) {
  out[0] = y + x + r; out[1] = y - x + r;
  out[2] = y - x - r; out[3] = y + x - r;
  int peak = 1;
  for (int i = 0; i < 4; ++i) if (magnitude(out[i]) > peak) peak = magnitude(out[i]);
  for (int i = 0; i < 4; ++i) out[i] = out[i] * cap / peak;
}

struct WheelRamp {
  int value = 0, lastSign = 0;
  uint32_t zeroAt = 0;
  void stop(uint32_t now) {
    if (value) zeroAt = now;
    value = 0; // Preserve direction history across STOP and mode changes.
  }
  int step(int target, uint32_t now) {
    int sign = target > 0 ? 1 : (target < 0 ? -1 : 0);
    bool reversing = sign && lastSign && sign != lastSign;
    if (reversing && (value || uint32_t(now - zeroAt) < 300)) target = 0;
    int next = value;
    if (next < target) next += (target - next > 3 ? 3 : target - next);
    if (next > target) next -= (next - target > 3 ? 3 : next - target);
    if (value && !next) zeroAt = now;
    value = next;
    if (value) lastSign = value > 0 ? 1 : -1;
    return value;
  }
};
