#include "udp_client.h"
#include "ctrl_client.h"
#include "state_machine.h"
#include "modem.h"
#include "protocol.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "udp_client";

esp_err_t udp_client_init(void)
{
    ESP_LOGI(TAG, "udp_client_init: OK");
    return ESP_OK;
}

esp_err_t udp_send_audio(uint8_t session_id, uint8_t group_id,
                          uint16_t seq, uint16_t timestamp,
                          const uint8_t *opus_data, size_t opus_len)
{
    // UDPパケット: 6バイトヘッダ + opusペイロード
    uint8_t pkt[6 + 64];
    if (opus_len > 64) opus_len = 64;

    udp_header_t hdr;
    proto_build_udp_header(&hdr, session_id, UDP_TYPE_AUDIO, group_id, seq, timestamp);
    memcpy(pkt, &hdr, sizeof(hdr));
    memcpy(pkt + sizeof(hdr), opus_data, opus_len);

    return modem_udp_send(pkt, sizeof(hdr) + opus_len);
}

esp_err_t udp_send_keepalive(uint8_t session_id, uint8_t group_id)
{
    uint8_t pkt[6];
    udp_header_t hdr;
    proto_build_udp_header(&hdr, session_id, UDP_TYPE_KEEPALIVE, group_id, 0, 0);
    memcpy(pkt, &hdr, sizeof(hdr));
    return modem_udp_send(pkt, sizeof(hdr));
}

void udp_rx_task(void *arg)
{
    // TODO: Step 7で実装
    vTaskDelete(NULL);
}

void udp_tx_task(void *arg)
{
    ESP_LOGI(TAG, "udp_tx_task: started");

    // EVT_CONNECTED が立つまで待機
    xEventGroupWaitBits(g_system_events, EVT_CONNECTED,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    // UDP ソケットオープン
    esp_err_t ret = modem_udp_open(CONFIG_SERVER_IP, CONFIG_UDP_PORT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "udp_tx_task: UDP open failed, task exit");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "udp_tx_task: UDP open OK");

    TickType_t last_keepalive = xTaskGetTickCount();
    encoded_frame_t frame;

    while (1) {
        // 切断されたら再接続待ち
        if (!(xEventGroupGetBits(g_system_events) & EVT_CONNECTED)) {
            modem_udp_close();
            xEventGroupWaitBits(g_system_events, EVT_CONNECTED,
                                pdFALSE, pdTRUE, portMAX_DELAY);
            ret = modem_udp_open(CONFIG_SERVER_IP, CONFIG_UDP_PORT);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "udp_tx_task: UDP reopen failed");
                continue;
            }
            last_keepalive = xTaskGetTickCount();
            ESP_LOGI(TAG, "udp_tx_task: UDP reopened");
        }

        // エンコード済みフレームを最大20ms待つ
        if (xQueueReceive(g_encoded_tx_queue, &frame, pdMS_TO_TICKS(20)) == pdTRUE) {
            uint8_t sid    = g_session_id;
            uint8_t grp    = (uint8_t)CONFIG_GROUP_ID;
            udp_send_audio(sid, grp, frame.seq, frame.timestamp_ms,
                           frame.data, (size_t)frame.len);
            last_keepalive = xTaskGetTickCount(); // 音声送信でキープアライブ更新
        }

        // キープアライブ（25秒間隔）
        if ((xTaskGetTickCount() - last_keepalive) >= pdMS_TO_TICKS(CONFIG_UDP_KEEPALIVE_INTERVAL_MS)) {
            udp_send_keepalive(g_session_id, (uint8_t)CONFIG_GROUP_ID);
            last_keepalive = xTaskGetTickCount();
            ESP_LOGD(TAG, "UDP keepalive sent");
        }
    }
}
