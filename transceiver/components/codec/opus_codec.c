#include "opus_codec.h"
#include "state_machine.h"
#include "jitter_buf.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "opus.h"
#include <string.h>

static const char *TAG = "opus_codec";

static OpusEncoder *s_encoder = NULL;
static OpusDecoder *s_decoder = NULL;

esp_err_t opus_codec_init(void)
{
    int err;

    // エンコーダ初期化（8kHz, モノラル, VoIP用途）
    s_encoder = opus_encoder_create(CONFIG_AUDIO_SAMPLE_RATE, CONFIG_AUDIO_CHANNELS,
                                     OPUS_APPLICATION_VOIP, &err);
    if (err != OPUS_OK || s_encoder == NULL) {
        ESP_LOGE(TAG, "opus_encoder_create failed: %s", opus_strerror(err));
        return ESP_FAIL;
    }
    opus_encoder_ctl(s_encoder, OPUS_SET_BITRATE(CONFIG_OPUS_BITRATE));
    opus_encoder_ctl(s_encoder, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(s_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(s_encoder, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(s_encoder, OPUS_SET_PACKET_LOSS_PERC(5));
    // DTX: 無音区間を最小サイズ（1〜2B）のパケットで送信し、
    // ノイズゲートで除去しきれなかった低レベルノイズの帯域消費を抑える。
    // 受信側は DTX パケットを PLC で補完するため、途切れは最小限。
    opus_encoder_ctl(s_encoder, OPUS_SET_DTX(1));

    // デコーダ初期化（Step 7で使用）
    s_decoder = opus_decoder_create(CONFIG_AUDIO_SAMPLE_RATE, CONFIG_AUDIO_CHANNELS, &err);
    if (err != OPUS_OK || s_decoder == NULL) {
        ESP_LOGE(TAG, "opus_decoder_create failed: %s", opus_strerror(err));
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "opus_codec_init: OK (16kHz mono 16kbps complexity=5 FEC)");
    return ESP_OK;
}

int opus_codec_encode(const int16_t *pcm, size_t samples,
                       uint8_t *out, size_t out_len)
{
    if (s_encoder == NULL) return OPUS_INTERNAL_ERROR;
    return opus_encode(s_encoder, pcm, (int)samples, out, (opus_int32)out_len);
}

int opus_codec_decode(const uint8_t *in, size_t in_len,
                       int16_t *pcm, size_t max_samples)
{
    if (s_decoder == NULL) return OPUS_INTERNAL_ERROR;
    return opus_decode(s_decoder, in, (opus_int32)in_len, pcm, (int)max_samples, 0);
}

void opus_encode_task(void *arg)
{
    ESP_LOGI(TAG, "opus_encode_task: started");

    pcm_frame_t   pcm_frame;
    encoded_frame_t enc_frame;
    uint16_t seq = 0;
    uint32_t timestamp_ms = 0;

    while (1) {
        if (xQueueReceive(g_pcm_encode_queue, &pcm_frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        int64_t t0 = esp_timer_get_time();
        int encoded_len = opus_codec_encode(pcm_frame.samples, CONFIG_OPUS_FRAME_SAMPLES,
                                             enc_frame.data, sizeof(enc_frame.data));
        int64_t encode_us = esp_timer_get_time() - t0;

        if (encoded_len < 0) {
            ESP_LOGW(TAG, "opus_encode failed: %s", opus_strerror(encoded_len));
            continue;
        }

        enc_frame.len          = encoded_len;
        enc_frame.seq          = seq;
        enc_frame.timestamp_ms = (uint16_t)(timestamp_ms & 0xFFFF);
        timestamp_ms          += CONFIG_OPUS_FRAME_MS;

        // 50フレーム（約1秒）ごとにログ出力
        if (seq % 50 == 0) {
            ESP_LOGI(TAG, "encode: seq=%u len=%dB time=%lldus (budget=20000us)",
                     seq, encoded_len, encode_us);
        }
        seq++;

        if (xQueueSend(g_encoded_tx_queue, &enc_frame, 0) != pdTRUE) {
            ESP_LOGW(TAG, "encoded_tx_queue full, dropping frame seq=%u", enc_frame.seq);
        }
    }
}

void opus_decode_task(void *arg)
{
    ESP_LOGI(TAG, "opus_decode_task: started");

    // 疑似スタック（pseudostack）をタスク起動直後に事前確保する。
    // opus_encode_task が PTT 送信時に先に 60KB を確保すると、
    // その後のデコーダ確保が失敗して silk_decode_frame でクラッシュする。
    // ここで PLC 呼び出しを 1 回行い ALLOC_STACK を通過させることで、
    // エンコーダより先に 60KB を確保する。
    {
        int16_t warmup[CONFIG_OPUS_FRAME_SAMPLES];
        opus_codec_decode(NULL, 0, warmup, CONFIG_OPUS_FRAME_SAMPLES);
    }

    uint8_t    opus_buf[256];
    size_t     opus_len = 0;
    pcm_frame_t pcm_frame;
    uint32_t   decode_count = 0;

    while (1) {
        // EVT_CONNECTED が立つまで待機
        xEventGroupWaitBits(g_system_events, EVT_CONNECTED,
                            pdFALSE, pdTRUE, portMAX_DELAY);
        ESP_LOGI(TAG, "opus_decode_task: connected, starting decode loop");

        uint32_t plc_count = 0;  // 連続 PLC フレーム数

        while (xEventGroupGetBits(g_system_events) & EVT_CONNECTED) {
            // ジッタバッファが初期フレーム数に達するまで 1 フレーム周期ずつ待機
            if (!jitter_buf_ready()) {
                vTaskDelay(pdMS_TO_TICKS(CONFIG_OPUS_FRAME_MS));
                plc_count = 0;
                continue;
            }

            int n;
            if (jitter_buf_pop(opus_buf, &opus_len) == ESP_OK) {
                // 正常デコード
                n = opus_codec_decode(opus_buf, opus_len,
                                      pcm_frame.samples, CONFIG_OPUS_FRAME_SAMPLES);
                plc_count = 0;
            } else {
                // パケットロス: 最大 25 フレーム(500ms)まで Opus PLC で補完。
                // LTE-M 経路では 1 秒程度のバーストロスが発生するため、
                // 5 フレームでは不足して無音になる。Opus PLC は 500ms 程度まで
                // 実用的な品質を維持できる。超えたら送信終了とみなして待機。
                if (plc_count < 25) {
                    n = opus_codec_decode(NULL, 0,
                                          pcm_frame.samples, CONFIG_OPUS_FRAME_SAMPLES);
                    plc_count++;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(CONFIG_OPUS_FRAME_MS));
                    continue;
                }
            }

            if (n < 0) {
                ESP_LOGW(TAG, "opus_decode failed: %s (opus_len=%zu)", opus_strerror(n), opus_len);
                continue;
            }

            // 診断: 期待サンプル数と不一致なら警告
            if (n != CONFIG_OPUS_FRAME_SAMPLES) {
                ESP_LOGW(TAG, "opus_decode: got %d samples (expected %d) opus_len=%zu",
                         n, CONFIG_OPUS_FRAME_SAMPLES, opus_len);
            }

            // 50フレームごとにログ（ピーク振幅で音声レベルを確認）
            decode_count++;
            if (decode_count % 50 == 0) {
                int16_t peak = 0;
                for (int i = 0; i < n && i < CONFIG_OPUS_FRAME_SAMPLES; i++) {
                    int16_t v = pcm_frame.samples[i] < 0 ? -pcm_frame.samples[i] : pcm_frame.samples[i];
                    if (v > peak) peak = v;
                }
                ESP_LOGI(TAG, "decode: %u frames opus_len=%zu peak=%d",
                         decode_count, opus_len, peak);
            }

            // portMAX_DELAY で送信キューをブロック。
            // キューが満杯（4フレーム=80ms分）のとき opus_decode がここでスリープし、
            // Core 1 の IDLE1 が CPU を得て WDT をリセットできる。
            // i2s_playback_task が 20ms ごとにフレームを消費することで自然にペーシングされる。
            xQueueSend(g_pcm_playback_queue, &pcm_frame, portMAX_DELAY);
        }

        decode_count = 0;
        ESP_LOGI(TAG, "opus_decode_task: disconnected");
    }
}
