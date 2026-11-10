#include "console/console.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include <stdint.h>
#include <string.h>

#define RELAY_GPIO 26
#define REED_SWITCH_GPIO 27

#define DEFAULT_BLE_PASSKEY 123456
#define DEFAULT_OTA_PASSWORD 123456

#define RELAY_PULSE_MS 500

static volatile bool relay_pulse_pending = false;
static bool relay_active = false;
static int64_t relay_pulse_start_ms = 0;

static nvs_handle_t nvs_secrets_handle;
static uint32_t ble_passkey;
static uint32_t ota_password;

static const char *TAG = "main";

static const ble_uuid128_t svc_uuid =
    BLE_UUID128_INIT(0xa1, 0xb6, 0x98, 0x1f, 0x6a, 0x0d, 0x0b, 0x89, 0x33, 0x4f,
                     0x19, 0x1b, 0x00, 0x00, 0xaf, 0xc9);
static const ble_uuid128_t status_chr_uuid =
    BLE_UUID128_INIT(0xa1, 0xb6, 0x98, 0x1f, 0x6a, 0x0d, 0x0b, 0x89, 0x33, 0x4f,
                     0x19, 0x1b, 0x01, 0x00, 0xaf, 0xc9);
static const ble_uuid128_t control_chr_uuid =
    BLE_UUID128_INIT(0xa1, 0xb6, 0x98, 0x1f, 0x6a, 0x0d, 0x0b, 0x89, 0x33, 0x4f,
                     0x19, 0x1b, 0x02, 0x00, 0xaf, 0xc9);
static const ble_uuid128_t change_passkey_chr_uuid =
    BLE_UUID128_INIT(0xa1, 0xb6, 0x98, 0x1f, 0x6a, 0x0d, 0x0b, 0x89, 0x33, 0x4f,
                     0x19, 0x1b, 0x03, 0x00, 0xaf, 0xc9);
static const ble_uuid128_t change_ota_password_chr_uuid =
    BLE_UUID128_INIT(0xa1, 0xb6, 0x98, 0x1f, 0x6a, 0x0d, 0x0b, 0x89, 0x33, 0x4f,
                     0x19, 0x1b, 0x04, 0x00, 0xaf, 0xc9);

static uint16_t status_val_handle;

#define MAX_CONN_HANDLES 3

static uint16_t conn_handles[MAX_CONN_HANDLES];

static int status_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg) {
  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    uint8_t value = gpio_get_level(REED_SWITCH_GPIO) ? 0x01 : 0x00;
    os_mbuf_append(ctxt->om, &value, sizeof(value));
  }
  return 0;
}

static int control_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg) {
  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
    relay_pulse_pending = true;
  }
  return 0;
}

static int change_passkey_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                    struct ble_gatt_access_ctxt *ctxt,
                                    void *arg) {
  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
    return 0;
  }

  uint8_t payload[8];
  uint16_t payload_len;
  int rc =
      ble_hs_mbuf_to_flat(ctxt->om, payload, sizeof(payload), &payload_len);
  if (rc != 0 || payload_len != sizeof(payload)) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  uint32_t current_passkey;
  uint32_t new_passkey;
  memcpy(&current_passkey, payload, sizeof(new_passkey));
  memcpy(&new_passkey, payload + sizeof(current_passkey), sizeof(new_passkey));

  if (current_passkey != ble_passkey) {
    return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
  }

  ble_passkey = new_passkey;
  ESP_ERROR_CHECK(nvs_set_u32(nvs_secrets_handle, "ble_passkey", ble_passkey));
  ESP_ERROR_CHECK(nvs_commit(nvs_secrets_handle));
  return 0;
}

static int change_ota_password_access_cb(uint16_t conn_handle,
                                         uint16_t attr_handle,
                                         struct ble_gatt_access_ctxt *ctxt,
                                         void *arg) {
  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
    return 0;
  }

  uint8_t payload[8];
  uint16_t payload_len;
  int rc =
      ble_hs_mbuf_to_flat(ctxt->om, payload, sizeof(payload), &payload_len);
  if (rc != 0 || payload_len != sizeof(payload)) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  uint32_t current_password;
  uint32_t new_password;
  memcpy(&current_password, payload, sizeof(current_password));
  memcpy(&new_password, payload + sizeof(current_password),
         sizeof(new_password));

  if (current_password != ota_password) {
    return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
  }

  ota_password = new_password;
  ESP_ERROR_CHECK(
      nvs_set_u32(nvs_secrets_handle, "ota_password", ota_password));
  ESP_ERROR_CHECK(nvs_commit(nvs_secrets_handle));
  return 0;
}

static const struct ble_gatt_chr_def gatt_chrs[] = {
    {
        .uuid = &status_chr_uuid.u,
        .access_cb = status_access_cb,
        .val_handle = &status_val_handle,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC |
                 BLE_GATT_CHR_F_READ_AUTHEN | BLE_GATT_CHR_F_NOTIFY,
    },
    {
        .uuid = &control_chr_uuid.u,
        .access_cb = control_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
                 BLE_GATT_CHR_F_WRITE_AUTHEN,
    },
    {
        .uuid = &change_passkey_chr_uuid.u,
        .access_cb = change_passkey_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
                 BLE_GATT_CHR_F_WRITE_AUTHEN,
    },
    {
        .uuid = &change_ota_password_chr_uuid.u,
        .access_cb = change_ota_password_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
                 BLE_GATT_CHR_F_WRITE_AUTHEN,
    },
    {0},
};

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &svc_uuid.u,
        .characteristics = gatt_chrs,
    },
    {0},
};

void host_task(void *param) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

static int gap_event_handler(struct ble_gap_event *event, void *arg);

static void start_advertising(void) {
  const char *name = ble_svc_gap_device_name();

  struct ble_hs_adv_fields fields = {0};
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.name = (const uint8_t *)name;
  fields.name_len = strlen(name);
  fields.name_is_complete = 1;
  ble_gap_adv_set_fields(&fields);

  struct ble_gap_adv_params adv_params = {0};
  adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
  adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

  uint8_t own_addr_type;
  ble_hs_id_infer_auto(0, &own_addr_type);

  ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params,
                    gap_event_handler, NULL);
}

static void on_sync(void) { start_advertising(); }

static int gap_event_handler(struct ble_gap_event *event, void *arg) {
  switch (event->type) {
  case BLE_GAP_EVENT_CONNECT:
    ESP_LOGI(TAG, "BLE connect, status=%d", event->connect.status);
    if (event->connect.status == 0) {
      for (int i = 0; i < MAX_CONN_HANDLES; i++) {
        if (conn_handles[i] == BLE_HS_CONN_HANDLE_NONE) {
          conn_handles[i] = event->connect.conn_handle;
          break;
        }
      }
    }
    start_advertising();
    return 0;

  case BLE_GAP_EVENT_DISCONNECT:
    ESP_LOGI(TAG, "BLE disconnect, restarting advertising");
    for (int i = 0; i < MAX_CONN_HANDLES; i++) {
      if (conn_handles[i] == event->disconnect.conn.conn_handle) {
        conn_handles[i] = BLE_HS_CONN_HANDLE_NONE;
        break;
      }
    }
    start_advertising();
    return 0;

  case BLE_GAP_EVENT_PASSKEY_ACTION:
    if (event->passkey.params.action == BLE_SM_IOACT_DISP) {
      struct ble_sm_io pkey = {0};
      pkey.action = BLE_SM_IOACT_DISP;
      pkey.passkey = ble_passkey;
      ble_sm_inject_io(event->passkey.conn_handle, &pkey);
    }
    return 0;

  default:
    return 0;
  }
}

void app_main(void) {
  gpio_set_level(RELAY_GPIO, 1);

  gpio_config_t io_conf = {};

  io_conf.pin_bit_mask = 1ULL << RELAY_GPIO;
  io_conf.mode = GPIO_MODE_OUTPUT;
  gpio_config(&io_conf);

  io_conf.pin_bit_mask = 1ULL << REED_SWITCH_GPIO;
  io_conf.mode = GPIO_MODE_INPUT;
  io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&io_conf);

  for (int i = 0; i < MAX_CONN_HANDLES; i++) {
    conn_handles[i] = BLE_HS_CONN_HANDLE_NONE;
  }

  esp_err_t nvs_ret = nvs_flash_init();
  if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
      nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    nvs_ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(nvs_ret);

  ESP_ERROR_CHECK(nvs_open("secrets", NVS_READWRITE, &nvs_secrets_handle));

  esp_err_t passkey_err =
      nvs_get_u32(nvs_secrets_handle, "ble_passkey", &ble_passkey);
  if (passkey_err == ESP_ERR_NVS_NOT_FOUND) {
    ble_passkey = DEFAULT_BLE_PASSKEY;
    ESP_ERROR_CHECK(
        nvs_set_u32(nvs_secrets_handle, "ble_passkey", ble_passkey));
    ESP_ERROR_CHECK(nvs_commit(nvs_secrets_handle));
  } else {
    ESP_ERROR_CHECK(passkey_err);
  }

  esp_err_t ota_password_err =
      nvs_get_u32(nvs_secrets_handle, "ota_password", &ota_password);
  if (ota_password_err == ESP_ERR_NVS_NOT_FOUND) {
    ota_password = DEFAULT_OTA_PASSWORD;
    ESP_ERROR_CHECK(
        nvs_set_u32(nvs_secrets_handle, "ota_password", ota_password));
    ESP_ERROR_CHECK(nvs_commit(nvs_secrets_handle));
  } else {
    ESP_ERROR_CHECK(ota_password_err);
  }

  nimble_port_init();
  ble_svc_gap_init();
  ble_svc_gatt_init();
  ble_svc_gap_device_name_set("GarageDoor");

  int rc = ble_gatts_count_cfg(gatt_svcs);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gatts_count_cfg failed: %d", rc);
  }
  rc = ble_gatts_add_svcs(gatt_svcs);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gatts_add_svcs failed: %d", rc);
  }

  ble_hs_cfg.sync_cb = on_sync;
  ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_DISP_ONLY;
  ble_hs_cfg.sm_bonding = 1;
  ble_hs_cfg.sm_mitm = 1;
  ble_hs_cfg.sm_sc = 1;

  nimble_port_freertos_init(host_task);

  gpio_set_level(RELAY_GPIO, 0);
  vTaskDelay(pdMS_TO_TICKS(1000));
  gpio_set_level(RELAY_GPIO, 1);

  int previous_value = -1;
  int current_value = 0;
  while (1) {
    current_value = gpio_get_level(REED_SWITCH_GPIO);
    if (current_value != previous_value) {
      ESP_LOGI(TAG, "STATUS: %s", current_value ? "OPEN" : "CLOSED");
      for (int i = 0; i < MAX_CONN_HANDLES; i++) {
        if (conn_handles[i] != BLE_HS_CONN_HANDLE_NONE) {
          ble_gatts_notify(conn_handles[i], status_val_handle);
        }
      }
    }
    previous_value = current_value;

    if (relay_pulse_pending && !relay_active) {
      relay_pulse_pending = false;
      relay_active = true;
      relay_pulse_start_ms = esp_timer_get_time() / 1000;
      gpio_set_level(RELAY_GPIO, 0);
    }

    if (relay_active && (esp_timer_get_time() / 1000 - relay_pulse_start_ms) >=
                            RELAY_PULSE_MS) {
      gpio_set_level(RELAY_GPIO, 1);
      relay_active = false;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
