#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"


#define RELAY_GPIO 26
#define REED_SWITCH_GPIO 27

static const char *TAG = "main";


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
