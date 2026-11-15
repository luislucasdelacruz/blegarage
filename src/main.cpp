#include "NimBLEAdvertising.h"
#include "NimBLECharacteristic.h"
#include "NimBLEDevice.h"
#include "NimBLEServer.h"
#include "NimBLEService.h"
#include "esp32-hal-gpio.h"
#include "esp32-hal.h"
#include "nimble/nimble/host/include/host/ble_hs.h"
#include <Arduino.h>
#include <cstdint>

#define RELAY_GPIO 26
#define REED_SWITCH_GPIO 27

#define RELAY_PULSE_MS 500
#define DEFAULT_BLE_PASSKEY 123456

constexpr char kServiceUuid[] = "c9af0000-1b19-4f33-890b-0d6a1f98b6a1";
constexpr char kStatusCharUuid[] = "c9af0001-1b19-4f33-890b-0d6a1f98b6a1";
constexpr char kControlCharUuid[] = "c9af0002-1b19-4f33-890b-0d6a1f98b6a1";

NimBLECharacteristic *statusChar = nullptr;

volatile bool relayPulsePending = false;
bool relayActive = false;
uint32_t relayPulseStartMs = 0;

class StatusCallbacks : public NimBLECharacteristicCallbacks {
  void onRead(NimBLECharacteristic *pCharacteristic) override {
    uint8_t value = digitalRead(REED_SWITCH_GPIO) ? 0x01 : 0x00;
    pCharacteristic->setValue(&value, 1);
  }
};

class ControlCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pCharacteristic) override {
    relayPulsePending = true;
  }
};

class ServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *pServer, ble_gap_conn_desc *desc) override {
    NimBLEDevice::startAdvertising();
  }

  void onDisconnect(NimBLEServer *pServer) override {
    NimBLEDevice::startAdvertising();
  }
};

void setup() {
  Serial.begin(115200);

  digitalWrite(RELAY_GPIO, HIGH);
  pinMode(RELAY_GPIO, OUTPUT);
  pinMode(REED_SWITCH_GPIO, INPUT_PULLUP);

  NimBLEDevice::init("GarageDoor");

  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityPasskey(DEFAULT_BLE_PASSKEY);

  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  NimBLEService *service = server->createService(kServiceUuid);

  statusChar = service->createCharacteristic(
      kStatusCharUuid, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::READ_ENC |
                           NIMBLE_PROPERTY::READ_AUTHEN |
                           NIMBLE_PROPERTY::NOTIFY);
  statusChar->setCallbacks(new StatusCallbacks());
  uint8_t initialStatus = digitalRead(REED_SWITCH_GPIO) ? 0x01 : 0x00;
  statusChar->setValue(&initialStatus, 1);

  NimBLECharacteristic *controlChar = service->createCharacteristic(
      kControlCharUuid, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC |
                            NIMBLE_PROPERTY::WRITE_AUTHEN);
  controlChar->setCallbacks(new ControlCallbacks());

  service->start();

  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(kServiceUuid);
  advertising->setMinInterval(32);
  advertising->setMaxInterval(48);
  advertising->start();

  Serial.println("GarageDoor BLE service advertising.");
}

void loop() {
  static bool lastReedClosed = false;
  bool reedClosed = digitalRead(REED_SWITCH_GPIO) == LOW;
  if (reedClosed != lastReedClosed) {
    lastReedClosed = reedClosed;
    uint8_t value = reedClosed ? 0x00 : 0x01;
    statusChar->setValue(&value, 1);
    statusChar->notify();
    Serial.printf("STATUS: %s\n", reedClosed ? "CLOSED" : "OPEN");
  }

  if (relayPulsePending && !relayActive) {
    relayPulsePending = false;
    relayActive = true;
    relayPulseStartMs = millis();
    digitalWrite(RELAY_GPIO, LOW);
  }
  if (relayActive && millis() - relayPulseStartMs >= RELAY_PULSE_MS) {
    digitalWrite(RELAY_GPIO, HIGH);
    relayActive = false;
  }

  delay(50);
}
