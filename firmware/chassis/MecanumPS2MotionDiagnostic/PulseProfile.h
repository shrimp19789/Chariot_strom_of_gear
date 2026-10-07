#pragma once
#include <stdint.h>

// Duty envelope only: no speed feedback. Preserve each wheel's direction.
// Caller bounds peak to +/-192 and duration to 1000..10000ms.
inline int triangleDuty(int peak, uint32_t elapsed, uint32_t duration) {
  if (duration < 2 || elapsed >= duration) return 0;
  uint32_t up = duration / 2;
  if (elapsed <= up) return peak * int(elapsed) / int(up);
  return peak * int(duration - elapsed) / int(duration - up);
}
