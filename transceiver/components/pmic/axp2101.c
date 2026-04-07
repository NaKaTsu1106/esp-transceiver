#include "axp2101.h"
#include "config.h"
#include "driver/i2c.h"
#include "esp_log.h"

static const char *TAG = "axp2101";

esp_err_t axp2101_init(void)
{
    // TODO: Step 2で実装
    ESP_LOGI(TAG, "axp2101_init: stub");
    return ESP_OK;
}

esp_err_t axp2101_write(uint8_t reg, uint8_t val)
{
    // TODO: Step 2で実装
    return ESP_OK;
}

esp_err_t axp2101_read(uint8_t reg, uint8_t *val)
{
    // TODO: Step 2で実装
    return ESP_OK;
}
