#include "ctrl_client.h"
#include "modem.h"
#include "state_machine.h"
#include "config.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

// AT+CAOPEN エラーコード 0 = 成功、それ以外 = 失敗
// 連続失敗回数がこの閾値を超えたらネットワーク再構築を試みる
#define CAOPEN_FAIL_NETWORK_RETRY 2

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
    int caopen_fail_count = 0;

    while (1) {
        ESP_LOGI(TAG, "ctrl_task: opening UDP ctrl to %s:%d",
                 CONFIG_SERVER_IP, CONFIG_CTRL_PORT);

        if (modem_ctrl_open(CONFIG_SERVER_IP, CONFIG_CTRL_PORT) != ESP_OK) {
            caopen_fail_count++;
            ESP_LOGE(TAG, "ctrl_task: UDP open failed (%d times), retry in %ums",
                     caopen_fail_count, retry_ms);

            // CAOPEN が連続して失敗する場合、モデムリセットや CNACT タイムアウトにより
            // ネットワーク基盤が落ちている。LTE 登録から再構築する。
            if (caopen_fail_count >= CAOPEN_FAIL_NETWORK_RETRY) {
                ESP_LOGW(TAG, "ctrl_task: network may be down, re-activating...");
                modem_ensure_network();
                caopen_fail_count = 0;
            }

            vTaskDelay(pdMS_TO_TICKS(retry_ms));
            retry_ms = (retry_ms * 2 > CONFIG_CTRL_RETRY_MAX_MS)
                       ? CONFIG_CTRL_RETRY_MAX_MS : retry_ms * 2;
            continue;
        }
        caopen_fail_count = 0;

        // HELLO → HELLO_ACK
        // AT+CARECV は即時ポーリングのため、デッドライン付きループで到着を待つ
        bool connected = false;
        if (send_hello() != ESP_OK) {
            ESP_LOGE(TAG, "ctrl_task: HELLO send failed");
            modem_ctrl_close();
            vTaskDelay(pdMS_TO_TICKS(retry_ms));
            retry_ms = (retry_ms * 2 > CONFIG_CTRL_RETRY_MAX_MS)
                       ? CONFIG_CTRL_RETRY_MAX_MS : retry_ms * 2;
            continue;
        }

        TickType_t hello_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
        while (xTaskGetTickCount() < hello_deadline) {
            ctrl_msg_t ack;
            int rc = recv_one_msg(&ack, 200);
            if (rc > 0 && ack.type == MSG_HELLO_ACK && ack.payload_len >= 1) {
                g_session_id = ack.payload[0];
                ESP_LOGI(TAG, "HELLO_ACK: session_id=%d", g_session_id);
                connected = true;
                break;
            }
            if (rc < 0) break;  // エラー
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (!connected) {
            ESP_LOGW(TAG, "ctrl_task: HELLO_ACK timeout");
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

            // 受信（100ms タイムアウト）
            // 注意: モデムは CARECV に ~5ms で応答するため 500ms タイムアウトは
            // 実質ノーウェイトになり、ループがビジー状態でミューテックスを連続保持する。
            // タイムアウトを 100ms に抑え、かつ受信後に 80ms 解放して
            // udp_tx_task が音声フレームを送信できる時間を確保する。
            ctrl_msg_t rx_msg;
            int rc = recv_one_msg(&rx_msg, 100);
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
                }  else {
                    if (xQueueSend(g_ctrl_rx_queue, &rx_msg, 0) != pdTRUE) {
                        ESP_LOGW(TAG, "ctrl_task: ctrl_rx_queue full, dropping 0x%02X",
                                 rx_msg.type);
                    }
                }
            }
            // 80ms 待機。
            // modem_udp_send は flush + prompt待ち(~10ms) + OK待ち(~10ms) で
            // 1フレームあたり約20ms ミューテックスを保持する。ctrl が20ms ごとに
            // CARECV(~10ms)を発行すると音声フレームの送信サイクルを妨害する。
            // 80ms 待機にすることで udp_tx が 4フレーム分(~80ms)を連続送信できる
            // 時間を確保し、その後 ctrl が CARECV を 1 回実行する比率にする。
            vTaskDelay(pdMS_TO_TICKS(80));
        }

    reconnect:
        state_clear(EVT_CONNECTED);
        state_set(EVT_DISCONNECTED);
        g_session_id = 0;
        caopen_fail_count = 0;
        modem_ctrl_close();
        ESP_LOGI(TAG, "ctrl_task: retry in %ums", retry_ms);
        vTaskDelay(pdMS_TO_TICKS(retry_ms));
        retry_ms = (retry_ms * 2 > CONFIG_CTRL_RETRY_MAX_MS)
                   ? CONFIG_CTRL_RETRY_MAX_MS : retry_ms * 2;
    }
}
