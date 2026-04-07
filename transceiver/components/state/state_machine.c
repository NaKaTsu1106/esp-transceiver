#include "state_machine.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "state_machine";

EventGroupHandle_t g_system_events = NULL;

void state_machine_init(void)
{
    g_system_events = xEventGroupCreate();
    configASSERT(g_system_events);
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
    // TODO: Step 4で実装
    ESP_LOGI(TAG, "heartbeat_task: stub");
    vTaskDelete(NULL);
}
