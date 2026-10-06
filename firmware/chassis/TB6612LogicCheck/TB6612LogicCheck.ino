// Stage 2 only: VM and catapult power MUST remain disconnected.
// No enable, PWM, movement, or brake commands are implemented.
// GPIO reads below are MCU-pad digital levels, NOT remote terminal voltages.
// Reset/download intervals depend on the physical 10k STBY pulldown.
#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_system.h>
#include <string.h>

constexpr uint8_t STBY = 33, CATAPULT_EN = 14;
constexpr uint8_t INPUTS[] = {16, 17, 18, 19, 23, 32, 15, 5};
constexpr uint8_t CATAPULT_INPUTS[] = {25, 26, 27};
const char *const NAMES[] = {"FL_IN1", "FL_IN2", "RL_IN1", "RL_IN2",
                            "FR_IN1", "FR_IN2", "RR_IN1", "RR_IN2"};
char line[32];
size_t used = 0;
bool overflow = false;
uint32_t lastStatus = 0;

void configureLow(uint8_t pin) {
  // Preload output latch before enabling output; enable input for pad readback.
  gpio_set_level(static_cast<gpio_num_t>(pin), 0);
  pinMode(pin, OUTPUT); // Arduino core 3.x also registers GPIO ownership.
}

void stopOutputs() {
  digitalWrite(STBY, LOW);
  digitalWrite(CATAPULT_EN, LOW);
  for (uint8_t pin : INPUTS) digitalWrite(pin, LOW);
  for (uint8_t pin : CATAPULT_INPUTS) digitalWrite(pin, LOW);
}

void status() {
  bool allLow = digitalRead(STBY) == LOW && digitalRead(CATAPULT_EN) == LOW;
  for (uint8_t pin : INPUTS) allLow = allLow && digitalRead(pin) == LOW;
  for (uint8_t pin : CATAPULT_INPUTS) allLow = allLow && digitalRead(pin) == LOW;
  Serial.printf("STATUS TB6612LogicCheck v1 uptime_ms=%lu STBY33=%d EN14=%d pads_low=%d motion=UNAVAILABLE\n",
                static_cast<unsigned long>(millis()), digitalRead(STBY),
                digitalRead(CATAPULT_EN), int(allLow));
  for (size_t i = 0; i < sizeof(INPUTS); ++i) {
    Serial.printf("%s GPIO%u=%d%s", NAMES[i], INPUTS[i], digitalRead(INPUTS[i]),
                  i + 1 == sizeof(INPUTS) ? "\n" : " ");
  }
  if (!allLow) Serial.println("FAULT: pad readback high; inspect wiring with power removed.");
}

void handleLine() {
  stopOutputs();
  if (overflow) {
    Serial.println("ERR line too long; outputs disabled");
  } else if (used) {
    line[used] = '\0';
    if (!strcmp(line, "STATUS")) status();
    else if (!strcmp(line, "STOP")) {
      Serial.println("OK STOP; outputs disabled");
      status();
    } else Serial.println("ERR supported commands: STATUS, STOP; outputs disabled");
  }
  used = 0;
  overflow = false;
}

void setup() {
  configureLow(STBY);
  configureLow(CATAPULT_EN);
  for (uint8_t pin : INPUTS) configureLow(pin);
  for (uint8_t pin : CATAPULT_INPUTS) configureLow(pin);
  stopOutputs();
  Serial.begin(115200);
  Serial.printf("BOOT TB6612LogicCheck v1 reset_reason=%d\n", int(esp_reset_reason()));
  Serial.println("STAGE2 ONLY: keep VM disconnected. Measure VCC/PWM/STBY at BOTH boards.");
  Serial.println("Readback is digital MCU-pad state, not terminal voltage or continuity proof.");
  status();
}

void loop() {
  stopOutputs();
  // Bound parsing so serial flooding cannot monopolize a loop iteration.
  for (uint8_t count = 0; count < 64 && Serial.available(); ++count) {
    const char ch = char(Serial.read());
    if (ch == '\n' || ch == '\r') handleLine();
    else if (!overflow) {
      if (used < sizeof(line) - 1 && ch >= 32 && ch <= 126) line[used++] = ch;
      else overflow = true;
    }
  }
  if (millis() - lastStatus >= 1000) {
    lastStatus = millis();
    status();
  }
  delay(1);
}
