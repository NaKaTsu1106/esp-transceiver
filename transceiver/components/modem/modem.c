#include "modem.h"
#include "at_cmd.h"
#include "esp_log.h"

static const char *TAG = "modem";

esp_err_t modem_init(void)
{
    // TODO: Step 3で実装
    ESP_LOGI(TAG, "modem_init: stub");
    return ESP_OK;
}

esp_err_t modem_connect(void)
{
    // TODO: Step 3で実装
    ESP_LOGI(TAG, "modem_connect: stub");
    return ESP_OK;
}

esp_err_t modem_get_ip(char *ip_buf, size_t len)
{
    // TODO: Step 3で実装
    return ESP_OK;
}
