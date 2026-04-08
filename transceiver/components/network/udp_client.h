#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stddef.h>

esp_err_t udp_client_init(void);
esp_err_t udp_send_audio(uint8_t session_id, uint8_t group_id,
                          uint16_t seq, uint16_t timestamp,
                          const uint8_t *opus_data, size_t opus_len);
esp_err_t udp_send_keepalive(uint8_t session_id, uint8_t group_id);
void udp_rx_task(void *arg);
void udp_tx_task(void *arg);
