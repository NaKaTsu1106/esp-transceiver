#include "i2s_capture.h"
#include "state_machine.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include <string.h>
#include <math.h>

// 音質テスト用: 1 に設定するとマイクの代わりに 1kHz 正弦波を送信する
// テスト完了後は 0 に戻すこと
#define TEST_TONE_ENABLE    0
#define TEST_TONE_FREQ_HZ   1000
#define TEST_TONE_AMPLITUDE 16000  // int16_t の約半分。ゲインなしで直接キューに積む

static const char *TAG = "i2s_capture";

// INMP441 は感度 -26 dBFS と低いため、Opus エンコーダへの入力レベルを最大化する
#define MIC_GAIN_X  4

static inline int16_t gain_clamp(int16_t s, int gain)
{
    int32_t v = (int32_t)s * gain;
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

static i2s_chan_handle_t s_rx_chan = NULL;
static i2s_chan_handle_t s_tx_chan = NULL;

i2s_chan_handle_t i2s_get_tx_chan(void) { return s_tx_chan; }

esp_err_t i2s_capture_init(void)
{
    // INMP441 (RX) と PCM5102 (TX) は BCK/WS を共有するため、
    // 同一 I2S ペリフェラル (I2S_NUM_0) でフルデュプレックス初期化する。
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan));

    // RX: INMP441 マイク（Philips I2S, 32bit ステレオ、L ch のみ使用）
    i2s_std_config_t rx_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                         I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_I2S_BCK_PIN,
            .ws   = CONFIG_I2S_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din  = CONFIG_I2S_DATA_IN_PIN,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &rx_cfg));

    // TX: PCM5102A DAC（Philips I2S, 32bit ステレオ、L/R 同一信号）
    i2s_std_config_t tx_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                         I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = CONFIG_I2S_BCK_PIN,
            .ws   = CONFIG_I2S_WS_PIN,
            .dout = CONFIG_I2S_DATA_OUT_PIN,
            .din  = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &tx_cfg));

    ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));
    ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));

    ESP_LOGI(TAG, "i2s_capture_init: OK (BCK=%d WS=%d DIN=%d DOUT=%d full-duplex)",
             CONFIG_I2S_BCK_PIN, CONFIG_I2S_WS_PIN,
             CONFIG_I2S_DATA_IN_PIN, CONFIG_I2S_DATA_OUT_PIN);
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
    // L チャンネルのみ抽出: INMP441 は 24bit 左詰め → >> 16 で int16 → ゲイン適用
    for (size_t i = 0; i < pairs; i++) {
        buf[i] = gain_clamp((int16_t)(raw[i * 2] >> 16), MIC_GAIN_X);
    }
    if (bytes_read) *bytes_read = pairs * sizeof(int16_t);
    return ESP_OK;
}

void i2s_capture_task(void *arg)
{
#if TEST_TONE_ENABLE
    ESP_LOGW(TAG, "i2s_capture_task: TEST TONE MODE (%dHz amp=%d)",
             TEST_TONE_FREQ_HZ, TEST_TONE_AMPLITUDE);

    pcm_frame_t frame;
    float phase = 0.0f;
    const float phase_inc = 2.0f * (float)M_PI * TEST_TONE_FREQ_HZ / CONFIG_AUDIO_SAMPLE_RATE;

    while (1) {
        for (int i = 0; i < CONFIG_OPUS_FRAME_SAMPLES; i++) {
            frame.samples[i] = (int16_t)(TEST_TONE_AMPLITUDE * sinf(phase));
            phase += phase_inc;
            if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }

        if (xEventGroupGetBits(g_system_events) & EVT_FLOOR_GRANTED) {
            if (xQueueSend(g_pcm_encode_queue, &frame, 0) != pdTRUE) {
                ESP_LOGD(TAG, "pcm_encode_queue full, dropping frame");
            }
        }
        // マイク読み取りの代わりにフレーム周期分待機
        vTaskDelay(pdMS_TO_TICKS(CONFIG_OPUS_FRAME_MS));
    }

#else
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
#endif
}
