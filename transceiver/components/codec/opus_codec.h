#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

esp_err_t opus_codec_init(void);
int       opus_codec_encode(const int16_t *pcm, size_t samples,
                             uint8_t *out, size_t out_len);
int       opus_codec_decode(const uint8_t *in, size_t in_len,
                             int16_t *pcm, size_t max_samples);
void      opus_encode_task(void *arg);
void      opus_decode_task(void *arg);
