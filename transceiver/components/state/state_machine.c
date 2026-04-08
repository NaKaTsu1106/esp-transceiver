#include "state_machine.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "state_machine";

EventGroupHandle_t g_system_events  = NULL;
QueueHandle_t      g_ctrl_tx_queue  = NULL;
QueueHandle_t      g_ctrl_rx_queue  = NULL;

void state_machine_init(void)
{
    g_system_events = xEventGroupCreate();
    configASSERT(g_system_events);

    g_ctrl_tx_queue = xQueueCreate(CONFIG_CTRL_TX_QUEUE_LEN, sizeof(ctrl_msg_t));
    configASSERT(g_ctrl_tx_queue);

    g_ctrl_rx_queue = xQueueCreate(CONFIG_CTRL_RX_QUEUE_LEN, sizeof(ctrl_msg_t));
    configASSERT(g_ctrl_rx_queue);

    ESP_LOGI(TAG, "state_machine_init: OK");
}

void state_set(EventBits_t bits)
{
    xEventGroupSetBits(g_system_events, bits);
}

void state_clear(EventBits_t bits)
{
    xEventGroupClearBits(g_system_events, bits);
}

EventBits_t state_get(void)
{
    return xEventGroupGetBits(g_system_events);
}

void state_machine_task(void *arg)
{
    // TODO: Step 5で実装
    ESP_LOGI(TAG, "state_machine_task: stub");
    vTaskDelete(NULL);
}

void ptt_task(void *arg)
{
    // TODO: Step 5で実装
    ESP_LOGI(TAG, "ptt_task: stub");
    vTaskDelete(NULL);
}

void led_task(void *arg)
{
    // TODO: Step 2で実装
    ESP_LOGI(TAG, "led_task: stub");
    vTaskDelete(NULL);
}

void heartbeat_task(void *arg)
{
    ESP_LOGI(TAG, "heartbeat_task: started");

    // EVT_CONNECTED が立つまで待機
    xEventGroupWaitBits(g_system_events, EVT_CONNECTED,
                        pdFALSE,   // クリアしない
                        pdTRUE,
                        portMAX_DELAY);
    ESP_LOGI(TAG, "heartbeat_task: connected, starting heartbeat at %dms interval",
             CONFIG_HEARTBEAT_INTERVAL_MS);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_HEARTBEAT_INTERVAL_MS));

        // 切断されていたら待機に戻る
        if (!(xEventGroupGetBits(g_system_events) & EVT_CONNECTED)) {
            xEventGroupWaitBits(g_system_events, EVT_CONNECTED,
                                pdFALSE, pdTRUE, portMAX_DELAY);
        }

        ctrl_msg_t hb = { .type = MSG_HEARTBEAT, .payload_len = 0 };
        if (xQueueSend(g_ctrl_tx_queue, &hb, pdMS_TO_TICKS(1000)) != pdTRUE) {
            ESP_LOGW(TAG, "heartbeat_task: ctrl_tx_queue full");
        }
    }
}
