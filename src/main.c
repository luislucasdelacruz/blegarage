#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"


static const char *TAG = "main";


void app_main(void)
{
	while (1)
	{
		ESP_LOGI(TAG, "hello world");
		vTaskDelay(pdMS_TO_TICKS(1000));
	}
}
