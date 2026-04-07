#include "tcp_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "tcp_client";

esp_err_t tcp_client_init(void)
{
    // TODO: Step 4で実装
    ESP_LOGI(TAG, "tcp_client_init: stub");
    return ESP_OK;
}

esp_err_t tcp_send(const ctrl_msg_t *msg)
{
    // TODO: Step 4で実装
    return ESP_OK;
}

void tcp_task(void *arg)
{
    // TODO: Step 4で実装
    ESP_LOGI(TAG, "tcp_task: stub");
    vTaskDelete(NULL);
}
