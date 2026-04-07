#pragma once
#include "esp_err.h"

esp_err_t at_cmd_init(uint32_t baud_rate);
esp_err_t at_cmd_send(const char *cmd, const char *expect, uint32_t timeout_ms);
esp_err_t at_cmd_set_baud(uint32_t baud_rate);
