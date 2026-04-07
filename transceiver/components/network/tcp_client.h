#pragma once
#include "esp_err.h"
#include "protocol.h"

esp_err_t tcp_client_init(void);
esp_err_t tcp_send(const ctrl_msg_t *msg);
void tcp_task(void *arg);
