#pragma once
#include "esp_err.h"
#include <stddef.h>

// UART初期化（ドライバ未インストール時のみインストール、インストール済みは baud 変更のみ）
esp_err_t at_cmd_init(uint32_t baud_rate);

// ATコマンド送信 → レスポンス全文を resp に格納
esp_err_t at_cmd_query(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms);

// ATコマンド送信 → expect 文字列を含む場合 ESP_OK, "ERROR" 含む場合 ESP_FAIL, タイムアウト ESP_ERR_TIMEOUT
esp_err_t at_cmd_send(const char *cmd, const char *expect, uint32_t timeout_ms);

// ボーレート変更のみ（ドライバ再インストール不要）
esp_err_t at_cmd_set_baud(uint32_t baud_rate);
