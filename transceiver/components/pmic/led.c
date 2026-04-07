#include "led.h"
#include "axp2101.h"
#include "esp_log.h"

static const char *TAG = "led";

esp_err_t led_init(void)
{
    // TODO: Step 2で実装
    ESP_LOGI(TAG, "led_init: stub");
    return ESP_OK;
}

esp_err_t led_set(uint8_t mode)
{
    // TODO: Step 2で実装
    return ESP_OK;
}
