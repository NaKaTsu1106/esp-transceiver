#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>

// UART初期化（ドライバ未インストール時のみインストール、インストール済みは baud 変更のみ）
esp_err_t at_cmd_init(uint32_t baud_rate);

// ATコマンド送信 → レスポンス全文を resp に格納
// "OK\r\n", "ERROR\r\n", "> " のいずれかで読み取り終了
esp_err_t at_cmd_query(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms);

// ATコマンド送信 → expect 文字列を含む場合 ESP_OK, "ERROR" 含む場合 ESP_FAIL, タイムアウト ESP_ERR_TIMEOUT
esp_err_t at_cmd_send(const char *cmd, const char *expect, uint32_t timeout_ms);

// ボーレート変更のみ（ドライバ再インストール不要）
esp_err_t at_cmd_set_baud(uint32_t baud_rate);

// UARTに生バイト列を書き込む（AT+CASEND のデータ送信用）
int at_cmd_write_raw(const uint8_t *data, size_t len);

// UARTからちょうど len バイトを読み込む（AT+CARECV のバイナリデータ受信用）
esp_err_t at_cmd_read_raw(uint8_t *buf, size_t len, uint32_t timeout_ms);

// コマンドを送信せずに特定文字列が現れるまで行単位で読む
// buf に最初にマッチした行（+CARECV: N など）を格納して返す
esp_err_t at_cmd_wait_line(const char *prefix, char *buf, size_t buf_len, uint32_t timeout_ms);
