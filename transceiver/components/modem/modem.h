#pragma once
#include "esp_err.h"

esp_err_t modem_init(void);
esp_err_t modem_connect(void);
esp_err_t modem_get_ip(char *ip_buf, size_t len);
