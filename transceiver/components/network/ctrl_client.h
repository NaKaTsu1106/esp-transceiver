#pragma once
#include "esp_err.h"
#include "protocol.h"
#include <stdint.h>

// サーバーから割り当てられたセッションID（接続確立後に有効）
extern uint8_t g_session_id;

esp_err_t ctrl_send(const ctrl_msg_t *msg);
void ctrl_task(void *arg);
