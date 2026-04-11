#include "loopback_test.h"
#include "i2s_capture.h"
#include "i2s_playback.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "opus.h"
#include <string.h>

static const char *TAG = "loopback";

// スピーカー出力ゲイン（飽和クランプ付き）
// MIC_GAIN_X=8 後の音声ピークは ~1000-5000 程度なので 8 倍でフルスケール相当
#define LOOPBACK_SPK_GAIN  8

static inline int16_t spk_clamp(int16_t s, int gain)
{
    int32_t v = (int32_t)s * gain;
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

static void loopback_task(void *arg)
{
    ESP_LOGI(TAG, "loopback_task: start");

    // ---- Opus 初期化 ----
    int err;
    OpusEncoder *enc = opus_encoder_create(CONFIG_AUDIO_SAMPLE_RATE,
                                           CONFIG_AUDIO_CHANNELS,
                                           OPUS_APPLICATION_VOIP, &err);
    if (!enc) {
        ESP_LOGE(TAG, "encoder create failed: %s", opus_strerror(err));
        vTaskDelete(NULL);
        return;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(CONFIG_OPUS_BITRATE));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));

    OpusDecoder *dec = opus_decoder_create(CONFIG_AUDIO_SAMPLE_RATE,
                                           CONFIG_AUDIO_CHANNELS, &err);
    if (!dec) {
        ESP_LOGE(TAG, "decoder create failed: %s", opus_strerror(err));
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Opus ready: %dHz %dch %dbps complexity=5",
             CONFIG_AUDIO_SAMPLE_RATE, CONFIG_AUDIO_CHANNELS, CONFIG_OPUS_BITRATE);

    static int16_t pcm_in[CONFIG_OPUS_FRAME_SAMPLES];
    static int16_t pcm_out[CONFIG_OPUS_FRAME_SAMPLES];
    static uint8_t opus_buf[256];
    static int32_t tx_buf[CONFIG_OPUS_FRAME_SAMPLES * 2];   // L/R インターリーブ

    uint32_t frame_count = 0;

    while (1) {
        // ---- マイク読み取り（i2s_capture_read が MIC_GAIN_X を適用済み）----
        size_t bytes_read = 0;
        if (i2s_capture_read(pcm_in, CONFIG_OPUS_FRAME_SAMPLES, &bytes_read) != ESP_OK) {
            ESP_LOGW(TAG, "capture read failed");
            vTaskDelay(pdMS_TO_TICKS(CONFIG_OPUS_FRAME_MS));
            continue;
        }

        // ---- Opus エンコード ----
        int encoded_len = opus_encode(enc, pcm_in, CONFIG_OPUS_FRAME_SAMPLES,
                                      opus_buf, sizeof(opus_buf));
        if (encoded_len < 0) {
            ESP_LOGW(TAG, "encode error: %s", opus_strerror(encoded_len));
            continue;
        }

        // ---- Opus デコード ----
        int n = opus_decode(dec, opus_buf, encoded_len,
                            pcm_out, CONFIG_OPUS_FRAME_SAMPLES, 0);
        if (n < 0) {
            ESP_LOGW(TAG, "decode error: %s", opus_strerror(n));
            continue;
        }

        // ---- スピーカー出力（int16 → int32 左詰め、ステレオ）----
        for (int i = 0; i < n; i++) {
            int32_t s = (int32_t)spk_clamp(pcm_out[i], LOOPBACK_SPK_GAIN) << 16;
            tx_buf[i * 2]     = s;  // L
            tx_buf[i * 2 + 1] = s;  // R
        }
        size_t bytes_written = 0;
        i2s_channel_write(i2s_get_tx_chan(), tx_buf,
                          n * 2 * sizeof(int32_t),
                          &bytes_written, portMAX_DELAY);

        // 50フレーム(1秒)ごとに診断ログ
        frame_count++;
        if (frame_count % 50 == 0) {
            int16_t peak_in = 0, peak_out = 0;
            for (int i = 0; i < CONFIG_OPUS_FRAME_SAMPLES; i++) {
                int16_t vi = pcm_in[i]  < 0 ? -pcm_in[i]  : pcm_in[i];
                int16_t vo = pcm_out[i] < 0 ? -pcm_out[i] : pcm_out[i];
                if (vi > peak_in)  peak_in  = vi;
                if (vo > peak_out) peak_out = vo;
            }
            ESP_LOGI(TAG, "frame=%u enc=%dB dec=%d peak_in=%d peak_out=%d",
                     frame_count, encoded_len, n, peak_in, peak_out);
        }
    }
}

void loopback_run(void)
{
    ESP_LOGI(TAG, "=== LOOPBACK TEST MODE ===");
    ESP_LOGI(TAG, "  MIC (I2S1 BCK=%d WS=%d DIN=%d) -> Opus -> SPK (I2S0 BCK=%d WS=%d DOUT=%d)",
             CONFIG_I2S_RX_BCK_PIN, CONFIG_I2S_RX_WS_PIN, CONFIG_I2S_RX_DIN_PIN,
             CONFIG_I2S_TX_BCK_PIN, CONFIG_I2S_TX_WS_PIN, CONFIG_I2S_TX_DOUT_PIN);

    ESP_ERROR_CHECK(i2s_capture_init());
    ESP_ERROR_CHECK(i2s_playback_init());

    // Opus は内部バッファが大きいため 32KB スタックが必要
    xTaskCreatePinnedToCore(loopback_task, "loopback", 32768, NULL, 20, NULL, 1);
}
