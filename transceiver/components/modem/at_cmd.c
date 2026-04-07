#include "at_cmd.h"
#include "config.h"
#include "esp_log.h"

static const char *TAG = "at_cmd";

esp_err_t at_cmd_init(uint32_t baud_rate)
{
    // TODO: Step 3で実装
    ESP_LOGI(TAG, "at_cmd_init: stub (baud=%lu)", baud_rate);
    return ESP_OK;
}

esp_err_t at_cmd_send(const char *cmd, const char *expect, uint32_t timeout_ms)
{
    // TODO: Step 3で実装
    return ESP_OK;
}

esp_err_t at_cmd_set_baud(uint32_t baud_rate)
{
    // TODO: Step 3で実装
    return ESP_OK;
}
