#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

esp_err_t modem_init(void);
esp_err_t modem_connect(void);
esp_err_t modem_get_ip(char *ip_buf, size_t len);

// 制御チャンネル UDP API（コネクションID=0 固定、ポート6000）
esp_err_t modem_ctrl_open(const char *host, uint16_t port);
esp_err_t modem_ctrl_send(const uint8_t *data, size_t len);
// 受信したバイト数を *out_len に格納。データなし時は out_len=0 で ESP_OK を返す。
esp_err_t modem_ctrl_recv(uint8_t *buf, size_t max_len, size_t *out_len, uint32_t timeout_ms);
esp_err_t modem_ctrl_close(void);

// 音声チャンネル UDP API（コネクションID=1 固定、ポート6001）
esp_err_t modem_udp_open(const char *host, uint16_t port);
esp_err_t modem_udp_send(const uint8_t *data, size_t len);
esp_err_t modem_udp_close(void);
