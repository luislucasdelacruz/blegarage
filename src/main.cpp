#include "esp32-hal-gpio.h"
#include "esp32-hal.h"
#include <Arduino.h>

#define RELAY_GPIO 26
#define REED_SWITCH_GPIO 27

void setup() {
  Serial.begin(115200);

  digitalWrite(RELAY_GPIO, HIGH);
  pinMode(RELAY_GPIO, OUTPUT);
  pinMode(REED_SWITCH_GPIO, INPUT_PULLUP);

  digitalWrite(RELAY_GPIO, LOW);
  delay(1000);
  digitalWrite(RELAY_GPIO, HIGH);
}

void loop() {
  static int previousValue = -1;
  int currentValue = digitalRead(REED_SWITCH_GPIO);
  if (currentValue != previousValue) {
    Serial.printf("STATUS: %s\n", currentValue ? "OPEN" : "CLOSED");
  }
  previousValue = currentValue;
  delay(50);
}
