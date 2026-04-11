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

#define SPK_GAIN_X      8
#define BEEP_AMPLITUDE  12000  // ビープ振幅（int16スケール）: SPK_GAIN_X に依存しない固定値

// ビープ再生状態（i2s_playback_task 内で使用）
static int   s_beep_remain = 0;   // 残りサンプル数
static float s_beep_phase  = 0.0f;
static float s_beep_inc    = 0.0f;

// 遅延ビープ（音声再生完了後に鳴らす）
static bool          s_deferred_pending = false;
static beep_request_t s_deferred_req    = {0};

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
    while (1) {
        // 通常音声フレームを tx_buf に展開
        pcm_frame_t frame;
        bool got_audio = (xQueueReceive(g_pcm_playback_queue, &frame, pdMS_TO_TICKS(25)) == pdTRUE);
        if (got_audio) {
            for (int i = 0; i < CONFIG_OPUS_FRAME_SAMPLES; i++) {
                int32_t s = (int32_t)spk_clamp(frame.samples[i], SPK_GAIN_X) << 16;
                tx_buf[i * 2]     = s;
                tx_buf[i * 2 + 1] = s;
            }
        } else {
            memset(tx_buf, 0, sizeof(tx_buf));
        }

        // ビープリクエストを取得（再生中でなければ）
        if (s_beep_remain <= 0) {
            beep_request_t req;
            if (xQueueReceive(g_beep_queue, &req, 0) == pdTRUE) {
                if (req.deferred) {
                    // 音声再生完了まで待つ: 保留しておき got_audio==false 時に起動
                    s_deferred_pending = true;
                    s_deferred_req     = req;
                } else {
                    s_beep_remain = CONFIG_AUDIO_SAMPLE_RATE * req.duration_ms / 1000;
                    s_beep_inc    = 2.0f * (float)M_PI * req.freq_hz / CONFIG_AUDIO_SAMPLE_RATE;
                    s_beep_phase  = 0.0f;
                }
            }
            // 音声が途切れた（キュー空）タイミングで遅延ビープを起動
            if (s_deferred_pending && !got_audio) {
                s_beep_remain      = CONFIG_AUDIO_SAMPLE_RATE * s_deferred_req.duration_ms / 1000;
                s_beep_inc         = 2.0f * (float)M_PI * s_deferred_req.freq_hz / CONFIG_AUDIO_SAMPLE_RATE;
                s_beep_phase       = 0.0f;
                s_deferred_pending = false;
            }
        }

        // ビープを音声にミックス（飽和加算）
        if (s_beep_remain > 0) {
            for (int i = 0; i < CONFIG_OPUS_FRAME_SAMPLES && s_beep_remain > 0; i++, s_beep_remain--) {
                int32_t beep_val = (int32_t)((float)BEEP_AMPLITUDE * sinf(s_beep_phase)) << 16;
                int64_t l = (int64_t)tx_buf[i * 2]     + beep_val;
                int64_t r = (int64_t)tx_buf[i * 2 + 1] + beep_val;
                tx_buf[i * 2]     = (int32_t)(l >  0x7FFFFFFF ?  0x7FFFFFFF : l < -0x80000000LL ? -0x80000000LL : l);
                tx_buf[i * 2 + 1] = (int32_t)(r >  0x7FFFFFFF ?  0x7FFFFFFF : r < -0x80000000LL ? -0x80000000LL : r);
                s_beep_phase += s_beep_inc;
                if (s_beep_phase >= 2.0f * (float)M_PI) s_beep_phase -= 2.0f * (float)M_PI;
            }
        }

        i2s_channel_write(tx, tx_buf, sizeof(tx_buf), &bytes_written, portMAX_DELAY);
    }
#endif
}
