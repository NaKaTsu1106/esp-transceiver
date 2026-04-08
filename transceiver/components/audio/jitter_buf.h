#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

esp_err_t jitter_buf_init(void);
esp_err_t jitter_buf_push(const uint8_t *opus_data, size_t len, uint16_t seq);
esp_err_t jitter_buf_pop(uint8_t *opus_data, size_t *len);
bool      jitter_buf_ready(void);   // 再生開始できるか（START_FRAMES以上蓄積）
void      jitter_buf_flush(void);
