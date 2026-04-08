#include "tcp_client.h"
#include "modem.h"
#include "state_machine.h"
#include "config.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "tcp_client";

uint8_t g_session_id = 0;

esp_err_t tcp_client_init(void)
{
    // 初期化不要（tcp_task がすべて担う）
    return ESP_OK;
}

// TCP で ctrl_msg_t を送信するヘルパー
esp_err_t tcp_send(const ctrl_msg_t *msg)
{
    uint8_t buf[2 + 255];
    int len = proto_encode(msg, buf, sizeof(buf));
    if (len < 0) return ESP_ERR_INVALID_ARG;
    return modem_tcp_send(buf, (size_t)len);
}

// HELLOメッセージを生成して送信
static esp_err_t send_hello(void)
{
    // device_id: EFUSEから読んだMACアドレス下4バイト
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
    return tcp_send(&hello);
}

// HELLO_ACK を受信してセッションIDを取得
static esp_err_t recv_hello_ack(void)
{
    uint8_t raw[4];
    size_t  received = 0;

    // ヘッダー2バイト受信（最大10秒）
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(10000);
    while (received < 2 && xTaskGetTickCount() < deadline) {
        size_t got = 0;
        modem_tcp_recv(raw + received, 2 - received, &got, 1000);
        received += got;
    }
    if (received < 2) {
        ESP_LOGE(TAG, "HELLO_ACK: header timeout");
        return ESP_ERR_TIMEOUT;
    }

    uint8_t payload_len = raw[0];
    uint8_t msg_type    = raw[1];

    if (msg_type != MSG_HELLO_ACK) {
        ESP_LOGE(TAG, "HELLO_ACK: unexpected type 0x%02X", msg_type);
        return ESP_FAIL;
    }
    if (payload_len < 1 || payload_len > sizeof(raw)) {
        ESP_LOGE(TAG, "HELLO_ACK: invalid payload_len %u", payload_len);
        return ESP_FAIL;
    }

    // ペイロード受信
    received = 0;
    deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
    while (received < payload_len && xTaskGetTickCount() < deadline) {
        size_t got = 0;
        modem_tcp_recv(raw + received, payload_len - received, &got, 1000);
        received += got;
    }
    if (received < payload_len) {
        ESP_LOGE(TAG, "HELLO_ACK: payload timeout");
        return ESP_ERR_TIMEOUT;
    }

    g_session_id = raw[0];
    ESP_LOGI(TAG, "HELLO_ACK: session_id=%d", g_session_id);
    return ESP_OK;
}

// 1メッセージをポーリング受信（500ms タイムアウト）
// 戻り値: 1=受信あり, 0=データなし（タイムアウト）, -1=エラー
static int recv_one_msg(ctrl_msg_t *msg)
{
    // ヘッダー2バイトを受信試行（500ms でモデムが応答する時間を確保）
    uint8_t hdr[2];
    size_t  got = 0;
    modem_tcp_recv(hdr, 2, &got, 500);

    if (got == 0) return 0;   // データなし

    if (got < 2) {
        // まれに1バイトだけ返る場合：タイムアウト付きループで残りを取得
        TickType_t dl = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
        while (got < 2 && xTaskGetTickCount() < dl) {
            size_t got2 = 0;
            modem_tcp_recv(hdr + got, 2 - got, &got2, 500);
            got += got2;
        }
        if (got < 2) return -1;
    }

    uint8_t payload_len = hdr[0];
    msg->type        = hdr[1];
    msg->payload_len = payload_len;

    if (payload_len > 0) {
        size_t recv_total = 0;
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
        while (recv_total < payload_len && xTaskGetTickCount() < deadline) {
            size_t r = 0;
            modem_tcp_recv(msg->payload + recv_total,
                           payload_len - recv_total, &r, 500);
            recv_total += r;
        }
        if (recv_total < payload_len) return -1;
    }

    return 1;
}

void tcp_task(void *arg)
{
    ESP_LOGI(TAG, "tcp_task: started, waiting for EVT_MODEM_READY");

    // LTE-M接続完了を待つ
    xEventGroupWaitBits(g_system_events, EVT_MODEM_READY,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    uint32_t backoff_ms = 2000;   // 初回リトライ待ち

    while (1) {
        ESP_LOGI(TAG, "tcp_task: connecting to %s:%d", CONFIG_SERVER_IP, CONFIG_TCP_PORT);

        // TCP接続
        if (modem_tcp_open(CONFIG_SERVER_IP, CONFIG_TCP_PORT) != ESP_OK) {
            ESP_LOGE(TAG, "tcp_task: connect failed, retry in %ums", backoff_ms);
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = (backoff_ms * 2 > CONFIG_TCP_RECONNECT_MAX_MS)
                         ? CONFIG_TCP_RECONNECT_MAX_MS : backoff_ms * 2;
            continue;
        }
        backoff_ms = 2000;   // 接続成功でリセット

        // HELLO 送信
        if (send_hello() != ESP_OK) {
            ESP_LOGE(TAG, "tcp_task: HELLO send failed");
            modem_tcp_close();
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            continue;
        }

        // HELLO_ACK 受信
        if (recv_hello_ack() != ESP_OK) {
            ESP_LOGE(TAG, "tcp_task: HELLO_ACK failed");
            modem_tcp_close();
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            continue;
        }

        // 接続確立
        state_set(EVT_CONNECTED);
        state_clear(EVT_DISCONNECTED);
        ESP_LOGI(TAG, "tcp_task: connected, session_id=%d", g_session_id);

        // 送受信ループ
        while (1) {
            // 送信キューをドレイン
            ctrl_msg_t tx_msg;
            while (xQueueReceive(g_ctrl_tx_queue, &tx_msg, 0) == pdTRUE) {
                if (tcp_send(&tx_msg) != ESP_OK) {
                    ESP_LOGE(TAG, "tcp_task: send failed");
                    goto reconnect;
                }
            }

            // 受信
            ctrl_msg_t rx_msg;
            int rc = recv_one_msg(&rx_msg);
            if (rc < 0) {
                ESP_LOGE(TAG, "tcp_task: recv error, reconnecting");
                goto reconnect;
            } else if (rc > 0) {
                if (rx_msg.type == MSG_DISCONNECT) {
                    ESP_LOGI(TAG, "tcp_task: DISCONNECT from server");
                    goto reconnect;
                }
                // 受信メッセージをキューへ
                if (xQueueSend(g_ctrl_rx_queue, &rx_msg, 0) != pdTRUE) {
                    ESP_LOGW(TAG, "tcp_task: ctrl_rx_queue full, dropping msg 0x%02X",
                             rx_msg.type);
                }
            }
            // recv_one_msg が最大500msブロックするため追加遅延不要
        }

    reconnect:
        state_clear(EVT_CONNECTED);
        state_set(EVT_DISCONNECTED);
        g_session_id = 0;
        modem_tcp_close();
        ESP_LOGI(TAG, "tcp_task: reconnecting in %ums", backoff_ms);
        vTaskDelay(pdMS_TO_TICKS(backoff_ms));
        backoff_ms = (backoff_ms * 2 > CONFIG_TCP_RECONNECT_MAX_MS)
                     ? CONFIG_TCP_RECONNECT_MAX_MS : backoff_ms * 2;
    }
}
