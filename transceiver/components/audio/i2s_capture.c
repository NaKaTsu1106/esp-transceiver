#include "i2s_capture.h"
#include "i2s_playback.h"
#include "state_machine.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include <string.h>

// 音質テスト用: 1 に設定するとマイクの代わりに 1kHz 正弦波を送信する
// テスト完了後は 0 に戻すこと
#define TEST_TONE_ENABLE    0
#define TEST_TONE_FREQ_HZ   1000
#define TEST_TONE_AMPLITUDE 16000  // int16_t の約半分。ゲインなしで直接キューに積む

// マイクループバックテスト用: 1 に設定するとマイク音声をそのままスピーカーに出力する
// ネットワーク・Opus を完全にバイパスし、PCM1808 → PCM5102 の直結経路を確認できる。
// テスト完了後は 0 に戻すこと
#define TEST_MIC_LOOPBACK  0

static const char *TAG = "i2s_capture";

#define MIC_GAIN_X 4

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
    // TX: PCM5102A スピーカー出力（I2S_NUM_0、TX専用チャンネル）
    {
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &s_tx_chan, NULL));

        i2s_std_config_t tx_cfg = {
            .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_AUDIO_SAMPLE_RATE),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                             I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = CONFIG_I2S_TX_BCK_PIN,
                .ws   = CONFIG_I2S_TX_WS_PIN,
                .dout = CONFIG_I2S_TX_DOUT_PIN,
                .din  = I2S_GPIO_UNUSED,
            },
        };
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_tx_chan, &tx_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(s_tx_chan));
    }

    // INMP441 L/R ピンを GPIO 出力 LOW に固定 → LEFT チャンネル選択
    {
        gpio_config_t lr_cfg = {
            .pin_bit_mask = (1ULL << CONFIG_I2S_RX_LR_PIN),
            .mode         = GPIO_MODE_OUTPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&lr_cfg));
        gpio_set_level(CONFIG_I2S_RX_LR_PIN, 0);
    }

    // RX: INMP441 MEMS マイク入力（I2S_NUM_1、RX専用チャンネル）
    // INMP441 は MCLK 不要・24bit I2S Philips 出力。L/R ピン GND で LEFT チャンネルに出力。
    {
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
        ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &s_rx_chan));

        i2s_std_config_t rx_cfg = {
            .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(CONFIG_AUDIO_SAMPLE_RATE),
            // STEREO モードで受信し L チャンネルのみ抽出する。
            // MONO モードでは DMA がステレオレートで埋まり i2s_channel_read が
            // 10ms/frame で返るため、encode がボトルネックになり encoded_tx_queue が詰まる。
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                             I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = CONFIG_I2S_RX_BCK_PIN,
                .ws   = CONFIG_I2S_RX_WS_PIN,
                .dout = I2S_GPIO_UNUSED,
                .din  = CONFIG_I2S_RX_DIN_PIN,
            },
        };
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_rx_chan, &rx_cfg));
        ESP_ERROR_CHECK(i2s_channel_enable(s_rx_chan));
    }

    ESP_LOGI(TAG, "i2s_capture_init: OK TX(I2S0 BCK=%d WS=%d DOUT=%d) RX(I2S1 BCK=%d WS=%d DIN=%d)",
             CONFIG_I2S_TX_BCK_PIN, CONFIG_I2S_TX_WS_PIN, CONFIG_I2S_TX_DOUT_PIN,
             CONFIG_I2S_RX_BCK_PIN, CONFIG_I2S_RX_WS_PIN, CONFIG_I2S_RX_DIN_PIN);
    return ESP_OK;
}

esp_err_t i2s_capture_read(int16_t *buf, size_t samples, size_t *bytes_read)
{
    // ステレオ受信: L/R インターリーブ int32。INMP441 は LEFT チャンネルに出力（L/R=GND）。
    // INMP441 は 24bit I2S → 32bit スロット上位詰め → >> 16 で int16 → ゲイン適用
    static int32_t raw[CONFIG_OPUS_FRAME_SAMPLES * 2];
    size_t bytes_got = 0;

    esp_err_t ret = i2s_channel_read(s_rx_chan, raw,
                                      samples * 2 * sizeof(int32_t),
                                      &bytes_got, portMAX_DELAY);
    if (ret != ESP_OK) return ret;

    size_t pairs = bytes_got / (2 * sizeof(int32_t));
    for (size_t i = 0; i < pairs; i++) {
        buf[i] = gain_clamp((int16_t)(raw[i * 2] >> 16), MIC_GAIN_X);
    }
    if (bytes_read) *bytes_read = pairs * sizeof(int16_t);
    return ESP_OK;
}

void i2s_capture_task(void *arg)
{
#if TEST_MIC_LOOPBACK
    ESP_LOGW(TAG, "i2s_capture_task: MIC LOOPBACK MODE (INMP441 -> PCM5102, gain=%d)", MIC_GAIN_X);

    static int32_t lb_rx[CONFIG_OPUS_FRAME_SAMPLES * 2];
    static int32_t lb_tx[CONFIG_OPUS_FRAME_SAMPLES * 2];
    i2s_chan_handle_t tx = i2s_get_tx_chan();
    size_t bytes_got = 0, bytes_written = 0;

    while (1) {
        esp_err_t ret = i2s_channel_read(s_rx_chan, lb_rx,
                                         sizeof(lb_rx), &bytes_got, portMAX_DELAY);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "loopback read failed: %s", esp_err_to_name(ret));
            continue;
        }
        size_t pairs = bytes_got / (2 * sizeof(int32_t));
        for (size_t i = 0; i < pairs; i++) {
            // INMP441: L チャンネル 24bit 上位詰め → >> 16 で int16 → ゲイン → PCM5102 用に << 16
            int16_t s = gain_clamp((int16_t)(lb_rx[i * 2] >> 16), MIC_GAIN_X);
            lb_tx[i * 2]     = (int32_t)s << 16;  // L
            lb_tx[i * 2 + 1] = (int32_t)s << 16;  // R
        }
        i2s_channel_write(tx, lb_tx, pairs * 2 * sizeof(int32_t),
                          &bytes_written, portMAX_DELAY);
    }

#elif TEST_TONE_ENABLE
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
