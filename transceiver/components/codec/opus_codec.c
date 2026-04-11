#include "opus_codec.h"
#include "state_machine.h"
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
    opus_encoder_ctl(s_encoder, OPUS_SET_PACKET_LOSS_PERC(10));

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
    // TODO: Step 7で実装
    vTaskDelete(NULL);
}
