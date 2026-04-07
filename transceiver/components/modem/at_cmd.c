#include "at_cmd.h"
#include "config.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "at_cmd";

#define AT_RX_BUF_SIZE  1024   // UARTドライバ内部バッファ

esp_err_t at_cmd_init(uint32_t baud_rate)
{
    uart_config_t cfg = {
        .baud_rate  = baud_rate,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    esp_err_t ret = uart_param_config(CONFIG_MODEM_UART_NUM, &cfg);
    if (ret != ESP_OK) return ret;

    ret = uart_set_pin(CONFIG_MODEM_UART_NUM,
                       CONFIG_MODEM_TXD_PIN, CONFIG_MODEM_RXD_PIN,
                       UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (ret != ESP_OK) return ret;

    if (uart_is_driver_installed(CONFIG_MODEM_UART_NUM)) {
        // 既インストール済み → ボーレートのみ変更
        return uart_set_baudrate(CONFIG_MODEM_UART_NUM, baud_rate);
    }
    return uart_driver_install(CONFIG_MODEM_UART_NUM, AT_RX_BUF_SIZE, 0, 0, NULL, 0);
}

esp_err_t at_cmd_set_baud(uint32_t baud_rate)
{
    return uart_set_baudrate(CONFIG_MODEM_UART_NUM, baud_rate);
}

esp_err_t at_cmd_query(const char *cmd, char *resp, size_t resp_len, uint32_t timeout_ms)
{
    uart_flush_input(CONFIG_MODEM_UART_NUM);

    // コマンド送信
    char tx[256];
    int tx_len = snprintf(tx, sizeof(tx), "%s\r\n", cmd);
    uart_write_bytes(CONFIG_MODEM_UART_NUM, tx, tx_len);
    ESP_LOGD(TAG, ">> %s", cmd);

    // レスポンス受信: "OK\r\n" or "ERROR\r\n" が来るまで読む
    size_t pos = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (xTaskGetTickCount() < deadline && pos < resp_len - 1) {
        int n = uart_read_bytes(CONFIG_MODEM_UART_NUM,
                                (uint8_t *)(resp + pos),
                                resp_len - pos - 1,
                                pdMS_TO_TICKS(50));
        if (n > 0) {
            pos += n;
            resp[pos] = '\0';
            // 終端検出
            if (strstr(resp, "OK\r\n") || strstr(resp, "ERROR\r\n") ||
                strstr(resp, "OK\n")   || strstr(resp, "ERROR\n")) {
                break;
            }
        }
    }
    resp[pos] = '\0';
    if (pos > 0) {
        ESP_LOGD(TAG, "<< %s", resp);
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t at_cmd_send(const char *cmd, const char *expect, uint32_t timeout_ms)
{
    char resp[512];
    esp_err_t ret = at_cmd_query(cmd, resp, sizeof(resp), timeout_ms);
    if (ret != ESP_OK) return ret;                      // タイムアウト（無応答）

    if (expect && strstr(resp, expect)) return ESP_OK;  // 期待文字列あり
    if (strstr(resp, "ERROR"))          return ESP_FAIL; // エラー応答
    return ESP_ERR_TIMEOUT;                              // 期待文字列なし
}
