#include "state_machine.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "led.h"

static const char *TAG = "state_machine";

// 合図音設定（周波数・長さ）
#define BEEP_FREQ_START_HZ  1000
#define BEEP_FREQ_STOP_HZ    800
#define BEEP_DURATION_MS      80

// ビープリクエストを g_beep_queue に投げる。
// 実際の正弦波生成・ミックスは i2s_playback_task が行う。
static void beep_request(int freq_hz, int duration_ms, bool deferred)
{
    beep_request_t req = { .freq_hz = freq_hz, .duration_ms = duration_ms, .deferred = deferred };
    if (xQueueSend(g_beep_queue, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "beep_request: queue full, dropping");
    }
}

EventGroupHandle_t g_system_events  = NULL;
QueueHandle_t      g_ctrl_tx_queue  = NULL;
QueueHandle_t      g_ctrl_rx_queue  = NULL;
QueueHandle_t      g_pcm_encode_queue   = NULL;
QueueHandle_t      g_encoded_tx_queue   = NULL;
QueueHandle_t      g_pcm_playback_queue = NULL;
QueueHandle_t      g_beep_queue         = NULL;

void state_machine_init(void)
{
    g_system_events = xEventGroupCreate();
    configASSERT(g_system_events);

    g_ctrl_tx_queue = xQueueCreate(CONFIG_CTRL_TX_QUEUE_LEN, sizeof(ctrl_msg_t));
    configASSERT(g_ctrl_tx_queue);

    g_ctrl_rx_queue = xQueueCreate(CONFIG_CTRL_RX_QUEUE_LEN, sizeof(ctrl_msg_t));
    configASSERT(g_ctrl_rx_queue);

    g_pcm_encode_queue = xQueueCreate(CONFIG_PCM_ENCODE_QUEUE_LEN, sizeof(pcm_frame_t));
    configASSERT(g_pcm_encode_queue);

    g_encoded_tx_queue = xQueueCreate(CONFIG_ENCODED_TX_QUEUE_LEN, sizeof(encoded_frame_t));
    configASSERT(g_encoded_tx_queue);

    g_pcm_playback_queue = xQueueCreate(CONFIG_PCM_PLAYBACK_QUEUE_LEN, sizeof(pcm_frame_t));
    configASSERT(g_pcm_playback_queue);

    g_beep_queue = xQueueCreate(2, sizeof(beep_request_t));
    configASSERT(g_beep_queue);

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
    ESP_LOGI(TAG, "state_machine_task: started");

    ctrl_msg_t msg;
    while (1) {
        if (xQueueReceive(g_ctrl_rx_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        ESP_LOGI(TAG, "state_machine: ctrl rx type=0x%02X", msg.type);
        switch (msg.type) {
        case MSG_PTT_START_ACK:
            if (xEventGroupGetBits(g_system_events) & EVT_PTT_PRESSED) {
                ESP_LOGI(TAG, "PTT_START_ACK: floor granted");
                xEventGroupClearBits(g_system_events, EVT_FLOOR_BUSY | EVT_FLOOR_FREE);
                xEventGroupSetBits(g_system_events, EVT_FLOOR_GRANTED);
                beep_request(BEEP_FREQ_START_HZ, BEEP_DURATION_MS, false);  // 送話開始合図
            } else {
                // PTT がすでに解放済み: ACK が遅延して到着したケース
                // MSG_PTT_STOP はすでに送信済みのため FLOOR_GRANTED はセットしない
                ESP_LOGW(TAG, "PTT_START_ACK: PTT already released, discarding late ACK");
            }
            break;

        case MSG_PTT_START_DENY:
            ESP_LOGI(TAG, "PTT_START_DENY: floor busy");
            // EVT_FLOOR_BUSY は MSG_PTT_NOTIFY/NOTIFY_STOP のみが管理する。
            // DENY は送信権取得失敗を表すだけで受信状態を変えない。
            // ここで FLOOR_BUSY をセットすると NOTIFY_STOP が来るまで LED が点滅し続ける。
            break;

        case MSG_PTT_NOTIFY:
            ESP_LOGI(TAG, "PTT_NOTIFY: remote TX started (session=%d)",
                     msg.payload_len > 0 ? msg.payload[0] : 0);
            xEventGroupClearBits(g_system_events, EVT_FLOOR_GRANTED | EVT_FLOOR_FREE);
            xEventGroupSetBits(g_system_events, EVT_FLOOR_BUSY);
            beep_request(BEEP_FREQ_START_HZ, BEEP_DURATION_MS, false);  // 受信開始合図
            break;

        case MSG_PTT_NOTIFY_STOP:
            ESP_LOGI(TAG, "PTT_NOTIFY_STOP: remote TX stopped");
            xEventGroupClearBits(g_system_events, EVT_FLOOR_GRANTED | EVT_FLOOR_BUSY);
            xEventGroupSetBits(g_system_events, EVT_FLOOR_FREE);
            beep_request(BEEP_FREQ_STOP_HZ, BEEP_DURATION_MS, true);  // 受信終了: 音声再生完了後に鳴らす
            break;

        case MSG_GROUP_CHANGE_ACK:
            ESP_LOGI(TAG, "GROUP_CHANGE_ACK: new group=%d",
                     msg.payload_len > 0 ? msg.payload[0] : 0);
            break;

        default:
            ESP_LOGW(TAG, "state_machine_task: unknown msg 0x%02X", msg.type);
            break;
        }
    }
}

void ptt_task(void *arg)
{
    // GPIO14 内部プルアップ入力（LOW = 押下）
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << CONFIG_PTT_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&io_cfg));

    ESP_LOGI(TAG, "ptt_task: GPIO%d pull-up hold=%dms", CONFIG_PTT_GPIO, CONFIG_PTT_HOLD_MS);

    bool pressed = false;

    // EVT_CONNECTED が立つまで待機
    xEventGroupWaitBits(g_system_events, EVT_CONNECTED,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    while (1) {
        // 切断中は待機
        if (!(xEventGroupGetBits(g_system_events) & EVT_CONNECTED)) {
            pressed = false;
            xEventGroupWaitBits(g_system_events, EVT_CONNECTED,
                                pdFALSE, pdTRUE, portMAX_DELAY);
        }

        bool current = (gpio_get_level(CONFIG_PTT_GPIO) == 0);

        if (current && !pressed) {
            // LOW を検出: CONFIG_PTT_HOLD_MS 後も継続していれば押下確定（チャタリング除去）
            vTaskDelay(pdMS_TO_TICKS(CONFIG_PTT_HOLD_MS));
            if (gpio_get_level(CONFIG_PTT_GPIO) != 0) {
                continue; // チャタリング
            }
            pressed = true;
            ESP_LOGI(TAG, "ptt_task: PTT pressed");
            xEventGroupSetBits(g_system_events, EVT_PTT_PRESSED);
            ctrl_msg_t m = { .type = MSG_PTT_START, .payload_len = 0 };
            if (xQueueSend(g_ctrl_tx_queue, &m, pdMS_TO_TICKS(1000)) == pdTRUE) {
                ESP_LOGI(TAG, "ptt_task: MSG_PTT_START queued");
            } else {
                ESP_LOGW(TAG, "ptt_task: ctrl_tx_queue full");
            }
        } else if (!current && pressed) {
            pressed = false;
            ESP_LOGI(TAG, "ptt_task: PTT released");
            xEventGroupClearBits(g_system_events, EVT_PTT_PRESSED | EVT_FLOOR_GRANTED);
            beep_request(BEEP_FREQ_STOP_HZ, BEEP_DURATION_MS, false);  // 送話終了合図
            ctrl_msg_t m = { .type = MSG_PTT_STOP, .payload_len = 0 };
            if (xQueueSend(g_ctrl_tx_queue, &m, pdMS_TO_TICKS(1000)) != pdTRUE) {
                ESP_LOGW(TAG, "ptt_task: ctrl_tx_queue full");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10)); // 10ms ポーリング
    }
}

void led_task(void *arg)
{
    ESP_LOGI(TAG, "led_task: started");

    uint8_t current_mode = CHGLED_1HZ; // 起動直後は1Hz点滅（main.cで設定済み）

    while (1) {
        EventBits_t bits = xEventGroupGetBits(g_system_events);

        uint8_t next_mode;
        if (bits & EVT_FLOOR_GRANTED) {
            next_mode = CHGLED_4HZ;   // 送話中: 4Hz点滅
        } else if (bits & EVT_FLOOR_BUSY) {
            next_mode = CHGLED_1HZ;   // 受話中: 1Hz点滅（2Hzモードは存在しないため）
        } else if (bits & EVT_CONNECTED) {
            next_mode = CHGLED_ON;    // 待機中: 常時点灯
        } else {
            next_mode = CHGLED_1HZ;   // 接続エラー・再接続中: 1Hz点滅
        }

        if (next_mode != current_mode) {
            esp_err_t ret = led_set(next_mode);
            if (ret == ESP_OK) {
                current_mode = next_mode;
                ESP_LOGI(TAG, "led_task: mode -> 0x%02X", next_mode);
            } else {
                ESP_LOGW(TAG, "led_task: led_set failed: %s", esp_err_to_name(ret));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100)); // 100ms ポーリング
    }
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
