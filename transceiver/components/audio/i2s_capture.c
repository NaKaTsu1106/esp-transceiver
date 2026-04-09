#include "i2s_capture.h"
#include "state_machine.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include <string.h>

static const char *TAG = "i2s_capture";

static i2s_chan_handle_t s_rx_chan = NULL;

esp_err_t i2s_capture_init(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan));

    // INMP441: Philips I2S, 8kHz, 32bit, ステレオ受信（モノだとRスロットにゴミが混入）
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                         I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_I2S_BCK_PIN,
            .ws   = CONFIG_I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = CONFIG_I2S_DATA_IN_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));

    ESP_LOGI(TAG, "i2s_capture_init: OK (BCK=%d WS=%d DIN=%d)",
             CONFIG_I2S_BCK_PIN, CONFIG_I2S_WS_PIN, CONFIG_I2S_DATA_IN_PIN);
    return ESP_OK;
}

esp_err_t i2s_capture_read(int16_t *buf, size_t samples, size_t *bytes_read)
{
    // ステレオ受信: L/R インターリーブ int32 バッファ
    static int32_t raw[CONFIG_OPUS_FRAME_SAMPLES * 2];
    size_t bytes_got = 0;

    esp_err_t ret = i2s_channel_read(s_rx_chan, raw,
                                      samples * 2 * sizeof(int32_t),
                                      &bytes_got, portMAX_DELAY);
    if (ret != ESP_OK) return ret;

    size_t pairs = bytes_got / (2 * sizeof(int32_t));
    // L チャンネルのみ抽出: INMP441 は 24bit 左詰め → >> 16 で int16
    for (size_t i = 0; i < pairs; i++) {
        buf[i] = (int16_t)(raw[i * 2] >> 16);
    }
    if (bytes_read) *bytes_read = pairs * sizeof(int16_t);
    return ESP_OK;
}

void i2s_capture_task(void *arg)
{
    ESP_LOGI(TAG, "i2s_capture_task: started");

    pcm_frame_t frame;

    while (1) {
        size_t bytes_read = 0;
        esp_err_t ret = i2s_capture_read(frame.samples, CONFIG_OPUS_FRAME_SAMPLES, &bytes_read);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "i2s_capture_read failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // 送話権保持中のみキューへプッシュ
        if (xEventGroupGetBits(g_system_events) & EVT_FLOOR_GRANTED) {
            if (xQueueSend(g_pcm_encode_queue, &frame, 0) != pdTRUE) {
                ESP_LOGD(TAG, "pcm_encode_queue full, dropping frame");
            }
        }
    }
}
