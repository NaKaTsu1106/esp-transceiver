#include "ctrl_client.h"
#include "modem.h"
#include "state_machine.h"
#include "config.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "ctrl";

uint8_t g_session_id = 0;

// UDP で ctrl_msg_t を送信するヘルパー
esp_err_t ctrl_send(const ctrl_msg_t *msg)
{
    uint8_t buf[2 + 255];
    int len = proto_encode(msg, buf, sizeof(buf));
    if (len < 0) return ESP_ERR_INVALID_ARG;
    return modem_ctrl_send(buf, (size_t)len);
}

// HELLO メッセージを生成して送信
static esp_err_t send_hello(void)
{
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    uint32_t device_id = ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
                         ((uint32_t)mac[4] << 8)  |  (uint32_t)mac[5];

    ctrl_msg_t hello = {
        .type        = MSG_HELLO,
        .payload_len = 5,
    };
    hello.payload[0] = (device_id >> 24) & 0xFF;
    hello.payload[1] = (device_id >> 16) & 0xFF;
    hello.payload[2] = (device_id >>  8) & 0xFF;
    hello.payload[3] =  device_id        & 0xFF;
    hello.payload[4] = CONFIG_GROUP_ID;

    ESP_LOGI(TAG, "Sending HELLO: device_id=0x%08X group=%d", (unsigned)device_id, CONFIG_GROUP_ID);
    return ctrl_send(&hello);
}

// 1メッセージを受信（UDP は 1 recv = 1 パケット = 1 メッセージ）
// 戻り値: 1=受信あり, 0=データなし（タイムアウト）, -1=不正パケット
static int recv_one_msg(ctrl_msg_t *msg, uint32_t timeout_ms)
{
    uint8_t raw[2 + 255];
    size_t got = 0;
    esp_err_t ret = modem_ctrl_recv(raw, sizeof(raw), &got, timeout_ms);

    if (ret != ESP_OK || got == 0) return 0;
    if (got < 2) return -1;

    msg->payload_len = raw[0];
    msg->type        = raw[1];

    if (msg->payload_len > 0) {
        if (got < (size_t)(2 + msg->payload_len)) return -1;
        memcpy(msg->payload, raw + 2, msg->payload_len);
    }

    return 1;
}

void ctrl_task(void *arg)
{
    ESP_LOGI(TAG, "ctrl_task: started, waiting for EVT_MODEM_READY");

    xEventGroupWaitBits(g_system_events, EVT_MODEM_READY,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    uint32_t retry_ms = 2000;

    while (1) {
        ESP_LOGI(TAG, "ctrl_task: opening UDP ctrl to %s:%d",
                 CONFIG_SERVER_IP, CONFIG_CTRL_PORT);

        if (modem_ctrl_open(CONFIG_SERVER_IP, CONFIG_CTRL_PORT) != ESP_OK) {
            ESP_LOGE(TAG, "ctrl_task: UDP open failed, retry in %ums", retry_ms);
            vTaskDelay(pdMS_TO_TICKS(retry_ms));
            retry_ms = (retry_ms * 2 > CONFIG_CTRL_RETRY_MAX_MS)
                       ? CONFIG_CTRL_RETRY_MAX_MS : retry_ms * 2;
            continue;
        }

        // HELLO → HELLO_ACK（最大3回リトライ）
        bool connected = false;
        for (int attempt = 0; attempt < 3 && !connected; attempt++) {
            if (send_hello() != ESP_OK) {
                ESP_LOGE(TAG, "ctrl_task: HELLO send failed");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            ctrl_msg_t ack;
            int rc = recv_one_msg(&ack, 5000);
            if (rc > 0 && ack.type == MSG_HELLO_ACK && ack.payload_len >= 1) {
                g_session_id = ack.payload[0];
                ESP_LOGI(TAG, "HELLO_ACK: session_id=%d", g_session_id);
                connected = true;
            } else {
                ESP_LOGW(TAG, "ctrl_task: HELLO_ACK timeout (attempt %d/3)", attempt + 1);
            }
        }

        if (!connected) {
            modem_ctrl_close();
            vTaskDelay(pdMS_TO_TICKS(retry_ms));
            retry_ms = (retry_ms * 2 > CONFIG_CTRL_RETRY_MAX_MS)
                       ? CONFIG_CTRL_RETRY_MAX_MS : retry_ms * 2;
            continue;
        }

        retry_ms = 2000;
        state_set(EVT_CONNECTED);
        state_clear(EVT_DISCONNECTED);
        ESP_LOGI(TAG, "ctrl_task: connected, session_id=%d", g_session_id);

        // 送受信ループ
        while (1) {
            // 送信キューをドレイン
            ctrl_msg_t tx_msg;
            while (xQueueReceive(g_ctrl_tx_queue, &tx_msg, 0) == pdTRUE) {
                if (ctrl_send(&tx_msg) != ESP_OK) {
                    ESP_LOGE(TAG, "ctrl_task: send failed");
                    goto reconnect;
                }
            }

            // 受信（500ms タイムアウト）
            ctrl_msg_t rx_msg;
            int rc = recv_one_msg(&rx_msg, 500);
            if (rc < 0) {
                ESP_LOGE(TAG, "ctrl_task: recv error");
                goto reconnect;
            } else if (rc > 0) {
                if (rx_msg.type == MSG_DISCONNECT) {
                    ESP_LOGI(TAG, "ctrl_task: DISCONNECT from server");
                    goto reconnect;
                }
                // HELLO_ACK はハンドシェイク専用。リトライ時の遅延 ACK がここに
                // 届くことがあるが state_machine では不要なので捨てる。
                if (rx_msg.type == MSG_HELLO_ACK) {
                    ESP_LOGD(TAG, "ctrl_task: late HELLO_ACK ignored");
                    continue;
                }
                if (xQueueSend(g_ctrl_rx_queue, &rx_msg, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "ctrl_task: ctrl_rx_queue full, dropping 0x%02X",
                             rx_msg.type);
                }
            }
            // recv_one_msg が最大 500ms ブロックするため追加遅延不要
        }

    reconnect:
        state_clear(EVT_CONNECTED);
        state_set(EVT_DISCONNECTED);
        g_session_id = 0;
        modem_ctrl_close();
        ESP_LOGI(TAG, "ctrl_task: retry in %ums", retry_ms);
        vTaskDelay(pdMS_TO_TICKS(retry_ms));
        retry_ms = (retry_ms * 2 > CONFIG_CTRL_RETRY_MAX_MS)
                   ? CONFIG_CTRL_RETRY_MAX_MS : retry_ms * 2;
    }
}
