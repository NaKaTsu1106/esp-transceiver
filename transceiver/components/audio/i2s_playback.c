#include "i2s_playback.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "i2s_playback";

esp_err_t i2s_playback_init(void)
{
    // TODO: Step 7で実装（PCM5102, 8kHz, モノラル）
    ESP_LOGI(TAG, "i2s_playback_init: stub");
    return ESP_OK;
}

esp_err_t i2s_playback_write(const int16_t *buf, size_t samples)
{
    // TODO: Step 7で実装
    return ESP_OK;
}

void i2s_playback_task(void *arg)
{
    // TODO: Step 7で実装
    vTaskDelete(NULL);
}
