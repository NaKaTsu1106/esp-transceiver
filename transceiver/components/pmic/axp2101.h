#pragma once
#include "esp_err.h"

esp_err_t axp2101_init(void);
esp_err_t axp2101_write(uint8_t reg, uint8_t val);
esp_err_t axp2101_read(uint8_t reg, uint8_t *val);
