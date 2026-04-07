#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// イベントビット定義
#define EVT_MODEM_READY     (1 << 0)
#define EVT_CONNECTED       (1 << 1)
#define EVT_PTT_PRESSED     (1 << 2)
#define EVT_FLOOR_GRANTED   (1 << 3)
#define EVT_FLOOR_BUSY      (1 << 4)
#define EVT_FLOOR_FREE      (1 << 5)
#define EVT_DISCONNECTED    (1 << 6)

extern EventGroupHandle_t g_system_events;

void state_machine_init(void);
void state_set(EventBits_t bits);
void state_clear(EventBits_t bits);
EventBits_t state_get(void);
void state_machine_task(void *arg);
void ptt_task(void *arg);
void led_task(void *arg);
void heartbeat_task(void *arg);
