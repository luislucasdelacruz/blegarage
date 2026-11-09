#include <string.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "host/ble_gap.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "console/console.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "nvs_flash.h"


#define RELAY_GPIO 26
#define REED_SWITCH_GPIO 27

#define BLE_PASSKEY 123456

static const char *TAG = "main";

void host_task(void *param)
{
	nimble_port_run();
	nimble_port_freertos_deinit();
}

static int gap_event_handler(struct ble_gap_event *event, void *arg);

static void start_advertising(void)
{
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

	ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params, gap_event_handler, NULL);
}

static void on_sync(void)
{
	start_advertising();
}

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
	switch (event->type)
	{
	case BLE_GAP_EVENT_CONNECT:
		ESP_LOGI(TAG, "BLE connect, status=%d", event->connect.status);
		start_advertising();
		return 0;

	case BLE_GAP_EVENT_DISCONNECT:
		ESP_LOGI(TAG, "BLE disconnect, restarting advertising");
		start_advertising();
		return 0;

	case BLE_GAP_EVENT_PASSKEY_ACTION:
		if (event->passkey.params.action == BLE_SM_IOACT_DISP)
		{
			struct ble_sm_io pkey = {0};
			pkey.action = BLE_SM_IOACT_DISP;
			pkey.passkey = BLE_PASSKEY;
			ble_sm_inject_io(event->passkey.conn_handle, &pkey);
		}
		return 0;

	default:
		return 0;
	}
}

void app_main(void)
{
	gpio_set_level(RELAY_GPIO, 1);

	gpio_config_t io_conf = {};

	io_conf.pin_bit_mask = 1ULL << RELAY_GPIO;
	io_conf.mode = GPIO_MODE_OUTPUT;
	gpio_config(&io_conf);

	io_conf.pin_bit_mask = 1ULL << REED_SWITCH_GPIO;
	io_conf.mode = GPIO_MODE_INPUT;
	io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
	gpio_config(&io_conf);

	esp_err_t nvs_ret = nvs_flash_init();
	if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
	{
		ESP_ERROR_CHECK(nvs_flash_erase());
		nvs_ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(nvs_ret);

	nimble_port_init();
	ble_svc_gap_init();
	ble_svc_gatt_init();
	ble_svc_gap_device_name_set("GarageDoor");

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
	while (1)
	{
		current_value = gpio_get_level(REED_SWITCH_GPIO);
		if (current_value != previous_value)
		{
			ESP_LOGI(TAG, "STATUS: %s", current_value ? "OPEN" : "CLOSED");
		}
		previous_value = current_value;
		vTaskDelay(pdMS_TO_TICKS(50));
	}
}
