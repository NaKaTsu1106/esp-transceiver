#include "i2s_capture.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "i2s_capture";

esp_err_t i2s_capture_init(void)
{
    // TODO: Step 6で実装（INMP441, 8kHz, モノラル）
    ESP_LOGI(TAG, "i2s_capture_init: stub");
    return ESP_OK;
}

esp_err_t i2s_capture_read(int16_t *buf, size_t samples, size_t *bytes_read)
{
    // TODO: Step 6で実装
    return ESP_OK;
}

void i2s_capture_task(void *arg)
{
    // TODO: Step 6で実装
    vTaskDelete(NULL);
}
