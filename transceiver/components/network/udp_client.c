#include "udp_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "udp_client";

esp_err_t udp_client_init(void)
{
    // TODO: Step 6で実装
    ESP_LOGI(TAG, "udp_client_init: stub");
    return ESP_OK;
}

esp_err_t udp_send_audio(uint8_t session_id, uint8_t group_id,
                          uint16_t seq, uint16_t timestamp,
                          const uint8_t *opus_data, size_t opus_len)
{
    // TODO: Step 6で実装
    return ESP_OK;
}

esp_err_t udp_send_keepalive(uint8_t session_id, uint8_t group_id)
{
    // TODO: Step 6で実装
    return ESP_OK;
}

void udp_rx_task(void *arg)
{
    // TODO: Step 7で実装
    vTaskDelete(NULL);
}

void udp_tx_task(void *arg)
{
    // TODO: Step 6で実装
    vTaskDelete(NULL);
}
