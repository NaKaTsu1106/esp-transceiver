#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

esp_err_t i2s_capture_init(void);
esp_err_t i2s_capture_read(int16_t *buf, size_t samples, size_t *bytes_read);
void      i2s_capture_task(void *arg);
