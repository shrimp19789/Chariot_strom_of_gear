#pragma once

// v3 one-point estimate from 30s STARTMOVE FWD at hold duty 192, VM=6V.
// FL/RL/FR/RR counts: 119/8, 135/8, 101/8, 76/8; target is slowest RR.
// STARTMOVE supplies its explicit 300ms kick separately from these hold gains.
// FAILED powered v3 check: FL/RL/FR stopped early; only RR ran to expiry.
// Preserve for diagnostic reproduction only; compensation defaults OFF.
// It is not a speed controller; reverse and loaded motion need validation.
constexpr int TRIM_NUM[4]={76,76,76,1}, TRIM_DEN[4]={119,135,101,1};
constexpr int TRIM_MAX_DUTY=192;
inline int trimmedDuty(int wheel,int logicalDuty) {
  if(wheel<0 || wheel>=4) return 0;
  // The compensated logical domain is capped at 192, including PS2 targets.
  if(logicalDuty>192) logicalDuty=192;
  if(logicalDuty<-192) logicalDuty=-192;
  const int sign=logicalDuty<0?-1:1;
  const int magnitude=logicalDuty<0?-logicalDuty:logicalDuty;
  int value=(magnitude*TRIM_NUM[wheel]+TRIM_DEN[wheel]/2)/TRIM_DEN[wheel];
  if(value>TRIM_MAX_DUTY) value=TRIM_MAX_DUTY;
  return sign*value;
}
