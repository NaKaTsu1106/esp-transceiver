#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

esp_err_t i2s_playback_init(void);
esp_err_t i2s_playback_write(const int16_t *buf, size_t samples);
void      i2s_playback_task(void *arg);
