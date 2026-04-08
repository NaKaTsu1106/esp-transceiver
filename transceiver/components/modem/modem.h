#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

esp_err_t modem_init(void);
esp_err_t modem_connect(void);
esp_err_t modem_get_ip(char *ip_buf, size_t len);

// TCP ソケット API（SIM7080G AT+CA コマンド群）
// 同時に開けるのは 1 本（コネクションID=0 固定）
esp_err_t modem_tcp_open(const char *host, uint16_t port);
esp_err_t modem_tcp_send(const uint8_t *data, size_t len);
// ちょうど len バイト受信。受信したバイト数を *out_len に格納。
esp_err_t modem_tcp_recv(uint8_t *buf, size_t max_len, size_t *out_len, uint32_t timeout_ms);
esp_err_t modem_tcp_close(void);
