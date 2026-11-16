#include "NimBLEAdvertising.h"
#include "NimBLEAttValue.h"
#include "NimBLECharacteristic.h"
#include "NimBLEDevice.h"
#include "NimBLEServer.h"
#include "NimBLEService.h"
#include "esp32-hal-gpio.h"
#include "esp32-hal.h"
#include "esp_err.h"
#include "nimble/nimble/host/include/host/ble_hs.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <Arduino.h>
#include <cstdint>
#include <cstring>

#define RELAY_GPIO 26
#define REED_SWITCH_GPIO 27

#define RELAY_PULSE_MS 500
#define DEFAULT_BLE_PASSKEY 123456
#define DEFAULT_OTA_PASSWORD 123456

constexpr char kServiceUuid[] = "c9af0000-1b19-4f33-890b-0d6a1f98b6a1";
constexpr char kStatusCharUuid[] = "c9af0001-1b19-4f33-890b-0d6a1f98b6a1";
constexpr char kControlCharUuid[] = "c9af0002-1b19-4f33-890b-0d6a1f98b6a1";
constexpr char kChangePasskeyCharUuid[] =
    "c9af0003-1b19-4f33-890b-0d6a1f98b6a1";
constexpr char kChangeOtaPasswordCharUuuid[] =
    "c9af0004-1b19-4f33-890b-0d6a1f98b6a1";

NimBLECharacteristic *statusChar = nullptr;

volatile bool relayPulsePending = false;
bool relayActive = false;
uint32_t relayPulseStartMs = 0;

nvs_handle_t nvsSecretsHandle;
uint32_t blePasskey;
uint32_t otaPassword;

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

bool readSecretPayload(NimBLECharacteristic *pCharacteristic,
                       uint32_t *currentValue, uint32_t *newValue) {
  NimBLEAttValue value = pCharacteristic->getValue();
  if (value.size() == 0) {
    return false;
  }

  memcpy(currentValue, value.data(), sizeof(uint32_t));
  memcpy(newValue, value.data() + sizeof(uint32_t), sizeof(uint32_t));
  return true;
}

class ChangePasskeyCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pCharacteristic) override {
    uint32_t currentValue, newValue;
    if (!readSecretPayload(pCharacteristic, &currentValue, &newValue)) {
      return;
    }
    if (currentValue != blePasskey) {
      return;
    }
    blePasskey = newValue;
    NimBLEDevice::setSecurityPasskey(blePasskey);
    ESP_ERROR_CHECK(nvs_set_u32(nvsSecretsHandle, "ble_passkey", blePasskey));
    ESP_ERROR_CHECK(nvs_commit(nvsSecretsHandle));
  }
};

class ChangeOtaPasswordCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pCharacteristic) override {
    uint32_t currentValue, newValue;
    if (!readSecretPayload(pCharacteristic, &currentValue, &newValue)) {
      return;
    }
    if (currentValue != otaPassword) {
      return;
    }
    otaPassword = newValue;
    ESP_ERROR_CHECK(nvs_set_u32(nvsSecretsHandle, "ota_password", otaPassword));
    ESP_ERROR_CHECK(nvs_commit(nvsSecretsHandle));
  }
};

void setup() {
  Serial.begin(115200);

  esp_err_t nvsRet = nvs_flash_init();
  if (nvsRet == ESP_ERR_NVS_NO_FREE_PAGES ||
      nvsRet == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvsRet = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvsRet);

  ESP_ERROR_CHECK(nvs_open("secrets", NVS_READWRITE, &nvsSecretsHandle));

  esp_err_t passkeyErr =
      nvs_get_u32(nvsSecretsHandle, "ble_passkey", &blePasskey);
  if (passkeyErr == ESP_ERR_NVS_NOT_FOUND) {
    blePasskey = DEFAULT_BLE_PASSKEY;
    ESP_ERROR_CHECK(nvs_set_u32(nvsSecretsHandle, "ble_passkey", blePasskey));
    ESP_ERROR_CHECK(nvs_commit(nvsSecretsHandle));
  } else {
    ESP_ERROR_CHECK(passkeyErr);
  }

  esp_err_t otaPasswordErr =
      nvs_get_u32(nvsSecretsHandle, "ota_password", &otaPassword);
  if (otaPasswordErr == ESP_ERR_NVS_NOT_FOUND) {
    otaPassword = DEFAULT_OTA_PASSWORD;
    ESP_ERROR_CHECK(nvs_set_u32(nvsSecretsHandle, "ota_password", otaPassword));
    ESP_ERROR_CHECK(nvs_commit(nvsSecretsHandle));
  } else {
    ESP_ERROR_CHECK(otaPasswordErr);
  }

  digitalWrite(RELAY_GPIO, HIGH);
  pinMode(RELAY_GPIO, OUTPUT);
  pinMode(REED_SWITCH_GPIO, INPUT_PULLUP);

  NimBLEDevice::init("GarageDoor");

  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_ONLY);
  NimBLEDevice::setSecurityPasskey(blePasskey);

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

  NimBLECharacteristic *changePasskeyChar = service->createCharacteristic(
      kChangePasskeyCharUuid, NIMBLE_PROPERTY::WRITE |
                                  NIMBLE_PROPERTY::WRITE_ENC |
                                  NIMBLE_PROPERTY::WRITE_AUTHEN);
  changePasskeyChar->setCallbacks(new ChangePasskeyCallbacks());

  NimBLECharacteristic *changeOtaPasswordChar = service->createCharacteristic(
      kChangeOtaPasswordCharUuuid, NIMBLE_PROPERTY::WRITE |
                                       NIMBLE_PROPERTY::WRITE_ENC |
                                       NIMBLE_PROPERTY::WRITE_AUTHEN);
  changeOtaPasswordChar->setCallbacks(new ChangeOtaPasswordCallbacks());

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
