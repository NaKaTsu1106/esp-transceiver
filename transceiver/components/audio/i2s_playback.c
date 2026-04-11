#include "i2s_playback.h"
#include "i2s_capture.h"
#include "state_machine.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <math.h>

// スピーカー出力テスト用: 1 に設定すると起動直後から 1kHz 正弦波を再生する
// テスト完了後は 0 に戻すこと
#define TEST_TONE_ENABLE    0
#define TEST_TONE_FREQ_HZ   1000
#define TEST_TONE_AMPLITUDE 16000

static const char *TAG = "i2s_playback";

// スピーカーゲイン（飽和クランプ付き）。
// MIC_GAIN_X=8 の音声入力が Opus encode/decode を経て届く場合、
// 音声ピーク振幅は概ね 1000〜5000 程度。
// SPK_GAIN_X=8 で 5000×8=40000 → 32767 クランプ = フルスケール相当。
// SPK_GAIN_X を上げすぎると正弦波が矩形波に化けて音が歪む。
#define SPK_GAIN_X  8

static inline int16_t spk_clamp(int16_t s, int gain)
{
    int32_t v = (int32_t)s * gain;
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

esp_err_t i2s_playback_init(void)
{
    // TX チャンネルは i2s_capture_init() で生成済み（フルデュプレックス）。
    if (i2s_get_tx_chan() == NULL) {
        ESP_LOGE(TAG, "i2s_playback_init: TX channel not ready (call i2s_capture_init first)");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "i2s_playback_init: OK (DOUT=%d)", CONFIG_I2S_TX_DOUT_PIN);
    return ESP_OK;
}

void i2s_playback_task(void *arg)
{
    ESP_LOGI(TAG, "i2s_playback_task: started");

    i2s_chan_handle_t tx = i2s_get_tx_chan();
    static int32_t tx_buf[CONFIG_OPUS_FRAME_SAMPLES * 2];
    size_t bytes_written;

#if TEST_TONE_ENABLE
    ESP_LOGW(TAG, "i2s_playback_task: TEST TONE MODE (%dHz amp=%d)",
             TEST_TONE_FREQ_HZ, TEST_TONE_AMPLITUDE);

    float phase = 0.0f;
    const float phase_inc = 2.0f * (float)M_PI * TEST_TONE_FREQ_HZ / CONFIG_AUDIO_SAMPLE_RATE;

    while (1) {
        for (int i = 0; i < CONFIG_OPUS_FRAME_SAMPLES; i++) {
            int32_t s = (int32_t)(TEST_TONE_AMPLITUDE * sinf(phase)) << 16;
            tx_buf[i * 2]     = s;
            tx_buf[i * 2 + 1] = s;
            phase += phase_inc;
            if (phase >= 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
        }
        i2s_channel_write(tx, tx_buf, sizeof(tx_buf), &bytes_written, portMAX_DELAY);
    }

#else
    static uint32_t play_data_count    = 0;
    static uint32_t play_silence_count = 0;

    while (1) {
        pcm_frame_t frame;
        if (xQueueReceive(g_pcm_playback_queue, &frame, pdMS_TO_TICKS(25)) == pdTRUE) {
            for (int i = 0; i < CONFIG_OPUS_FRAME_SAMPLES; i++) {
                int32_t s = (int32_t)spk_clamp(frame.samples[i], SPK_GAIN_X) << 16;
                tx_buf[i * 2]     = s;  // L
                tx_buf[i * 2 + 1] = s;  // R (同一)
            }
            play_data_count++;

            // 50フレームごとにピーク振幅を診断
            if (play_data_count % 50 == 0) {
                int16_t peak = 0;
                for (int i = 0; i < CONFIG_OPUS_FRAME_SAMPLES; i++) {
                    int16_t v = frame.samples[i] < 0 ? -frame.samples[i] : frame.samples[i];
                    if (v > peak) peak = v;
                }
                ESP_LOGI(TAG, "playback[%u]: peak=%d silence=%u",
                         play_data_count, peak, play_silence_count);
            }
        } else {
            // データなし: DMA バッファの残留ノイズを無音で上書き
            memset(tx_buf, 0, sizeof(tx_buf));
            play_silence_count++;

            // 無音が 50 フレーム連続したら警告（キュー枯渇の兆候）
            if (play_silence_count % 50 == 0 && play_data_count > 0) {
                ESP_LOGW(TAG, "playback: silence %u frames (data=%u)",
                         play_silence_count, play_data_count);
            }
        }

        i2s_channel_write(tx, tx_buf, sizeof(tx_buf), &bytes_written, portMAX_DELAY);
    }
#endif
}
