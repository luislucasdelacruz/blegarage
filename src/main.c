#include "console/console.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
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

#define OTA_CMD_START 0x01
#define OTA_CMD_END 0x02
#define OTA_CMD_ABORT 0x03

static esp_ota_handle_t ota_handle;
static const esp_partition_t *ota_update_partition = NULL;
static bool ota_in_progress = false;

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
static const ble_uuid128_t ota_control_chr_uuid =
    BLE_UUID128_INIT(0xa1, 0xb6, 0x98, 0x1f, 0x6a, 0x0d, 0x0b, 0x89, 0x33, 0x4f,
                     0x19, 0x1b, 0x05, 0x00, 0xaf, 0xc9);
static const ble_uuid128_t ota_data_chr_uuid =
    BLE_UUID128_INIT(0xa1, 0xb6, 0x98, 0x1f, 0x6a, 0x0d, 0x0b, 0x89, 0x33, 0x4f,
                     0x19, 0x1b, 0x06, 0x00, 0xaf, 0xc9);

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

static int ota_control_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                 struct ble_gatt_access_ctxt *ctxt, void *arg) {
  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
    return 0;
  }

  uint8_t payload[5];
  uint16_t payload_len;
  int rc =
      ble_hs_mbuf_to_flat(ctxt->om, payload, sizeof(payload), &payload_len);
  if (rc != 0 || payload_len < 1) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  uint8_t command = payload[0];

  if (command == OTA_CMD_START) {
    if (payload_len != sizeof(payload) || ota_in_progress) {
      return BLE_ATT_ERR_UNLIKELY;
    }

    uint32_t provided_password;
    memcpy(&provided_password, payload + 1, sizeof(provided_password));
    if (provided_password != ota_password) {
      return BLE_ATT_ERR_INSUFFICIENT_AUTHOR;
    }

    ota_update_partition = esp_ota_get_next_update_partition(NULL);
    if (ota_update_partition == NULL) {
      ESP_LOGE(TAG, "No OTA update partition available");
      return BLE_ATT_ERR_UNLIKELY;
    }

    esp_err_t err =
        esp_ota_begin(ota_update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_ota_begin failed: %d", err);
      return BLE_ATT_ERR_UNLIKELY;
    }

    ota_in_progress = true;
    ESP_LOGI(TAG, "OTA started, writing to partition '%s'",
             ota_update_partition->label);
  } else if (command == OTA_CMD_END) {
    if (!ota_in_progress) {
      return BLE_ATT_ERR_UNLIKELY;
    }

    esp_err_t err = esp_ota_end(ota_handle);
    ota_in_progress = false;
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_ota_end failed: %d", err);
      return BLE_ATT_ERR_UNLIKELY;
    }

    err = esp_ota_set_boot_partition(ota_update_partition);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %d", err);
      return BLE_ATT_ERR_UNLIKELY;
    }

    ESP_LOGI(TAG, "OTA complete, rebooting into new firmware");
    esp_restart();
  } else if (command == OTA_CMD_ABORT) {
    if (ota_in_progress) {
      esp_ota_abort(ota_handle);
      ota_in_progress = false;
      ESP_LOGI(TAG, "OTA aborted");
    }
  } else {
    return BLE_ATT_ERR_UNLIKELY;
  }

  return 0;
}

static int ota_data_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
  if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
    return 0;
  }

  if (!ota_in_progress) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  uint8_t chunk[512];
  uint16_t chunk_len;
  int rc = ble_hs_mbuf_to_flat(ctxt->om, chunk, sizeof(chunk), &chunk_len);
  if (rc != 0) {
    return BLE_ATT_ERR_UNLIKELY;
  }

  esp_err_t err = esp_ota_write(ota_handle, chunk, chunk_len);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_write failed: %d", err);
    return BLE_ATT_ERR_UNLIKELY;
  }

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
    {
        .uuid = &ota_control_chr_uuid.u,
        .access_cb = ota_control_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC |
                 BLE_GATT_CHR_F_WRITE_AUTHEN,
    },
    {
        .uuid = &ota_data_chr_uuid.u,
        .access_cb = ota_data_access_cb,
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
  fields.uuids128 = &svc_uuid;
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;
  ble_gap_adv_set_fields(&fields);

  struct ble_hs_adv_fields rsp_fields = {0};
  rsp_fields.name = (const uint8_t *)name;
  rsp_fields.name_len = strlen(name);
  rsp_fields.name_is_complete = 1;
  ble_gap_adv_rsp_set_fields(&rsp_fields);

  struct ble_gap_adv_params adv_params = {0};
  adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
  adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

  uint8_t own_addr_type;
  ble_hs_id_infer_auto(0, &own_addr_type);

  ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params,
                    gap_event_handler, NULL);
}

static void restore_resolving_list(void) {
  extern int ble_hs_pvcy_add_entry(const uint8_t *addr, uint8_t addrtype,
                                    const uint8_t *irk);

  ble_addr_t peer_addrs[CONFIG_BT_NIMBLE_MAX_BONDS];
  int num_peers;
  int rc = ble_store_util_bonded_peers(peer_addrs, &num_peers,
                                       CONFIG_BT_NIMBLE_MAX_BONDS);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_store_util_bonded_peers failed: %d", rc);
    return;
  }

  ESP_LOGI(TAG, "restore_resolving_list: found %d bonded peer(s)", num_peers);

  for (int i = 0; i < num_peers; i++) {
    struct ble_store_key_sec key_sec = {0};
    struct ble_store_value_sec value_sec;
    key_sec.peer_addr = peer_addrs[i];

    rc = ble_store_read_peer_sec(&key_sec, &value_sec);
    ESP_LOGI(TAG,
             "restore_resolving_list: peer %d addr_type=%d addr=%02x:%02x:%02x:%02x:%02x:%02x rc=%d irk_present=%d",
             i, peer_addrs[i].type, peer_addrs[i].val[0], peer_addrs[i].val[1],
             peer_addrs[i].val[2], peer_addrs[i].val[3], peer_addrs[i].val[4],
             peer_addrs[i].val[5], rc, value_sec.irk_present);
    if (rc == 0 && value_sec.irk_present) {
      ble_hs_pvcy_add_entry(value_sec.peer_addr.val, value_sec.peer_addr.type,
                            value_sec.irk);
    }
  }
}

static void on_sync(void) {
  restore_resolving_list();
  start_advertising();
  esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback failed: %d", err);
  }
}

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
    ESP_LOGW(TAG, "nvs_flash_init returned %d, erasing NVS entirely", nvs_ret);
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

  extern void ble_store_config_init(void);
  ble_store_config_init();

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
  ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

  nimble_port_freertos_init(host_task);

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
