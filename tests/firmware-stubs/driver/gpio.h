#pragma once
#include <Arduino.h>
typedef int gpio_num_t;
inline void gpio_set_level(gpio_num_t pin,int value) { simulatedPins[pin]=value; }
