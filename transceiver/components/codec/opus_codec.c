#include "opus_codec.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "opus_codec";

esp_err_t opus_codec_init(void)
{
    // TODO: Step 6で実装（8kHz, モノラル, 8kbps, esp-libopus使用）
    ESP_LOGI(TAG, "opus_codec_init: stub");
    return ESP_OK;
}

int opus_codec_encode(const int16_t *pcm, size_t samples,
                       uint8_t *out, size_t out_len)
{
    // TODO: Step 6で実装
    return 0;
}

int opus_codec_decode(const uint8_t *in, size_t in_len,
                       int16_t *pcm, size_t max_samples)
{
    // TODO: Step 7で実装
    return 0;
}

void opus_encode_task(void *arg)
{
    // TODO: Step 6で実装
    vTaskDelete(NULL);
}

void opus_decode_task(void *arg)
{
    // TODO: Step 7で実装
    vTaskDelete(NULL);
}
