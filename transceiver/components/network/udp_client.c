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

    while (1) {
        // EVT_CONNECTED が立つまで待機
        xEventGroupWaitBits(g_system_events, EVT_CONNECTED,
                            pdFALSE, pdTRUE, portMAX_DELAY);

        // UDP 音声チャンネルを開く
        ESP_LOGI(TAG, "udp_tx_task: opening UDP audio %s:%d",
                 CONFIG_SERVER_IP, CONFIG_UDP_PORT);
        if (modem_udp_open(CONFIG_SERVER_IP, CONFIG_UDP_PORT) != ESP_OK) {
            ESP_LOGE(TAG, "udp_tx_task: UDP open failed, retry in 2s");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        ESP_LOGI(TAG, "udp_tx_task: UDP audio channel open");

        encoded_frame_t frame;
        TickType_t last_ka    = xTaskGetTickCount();
        uint32_t total_frames = 0;

        while (xEventGroupGetBits(g_system_events) & EVT_CONNECTED) {
            // エンコード済みフレームを最大 20ms 待つ
            if (xQueueReceive(g_encoded_tx_queue, &frame, pdMS_TO_TICKS(20)) == pdTRUE) {
                esp_err_t ret = udp_send_audio(g_session_id, CONFIG_GROUP_ID,
                                               frame.seq, frame.timestamp_ms,
                                               frame.data, frame.len);
                if (ret == ESP_OK) {
                    total_frames++;
                    last_ka = xTaskGetTickCount();
                    // 50フレーム（約1秒）ごとに統計ログ
                    if (total_frames % 50 == 0) {
                        ESP_LOGI(TAG, "audio TX: %u frames seq=%u len=%dB",
                                 total_frames, frame.seq, frame.len);
                    }
                } else {
                    ESP_LOGW(TAG, "udp_send_audio failed seq=%u", frame.seq);
                }
            }

            // 25秒ごとにキープアライブ送信
            if (xTaskGetTickCount() - last_ka >
                    pdMS_TO_TICKS(CONFIG_UDP_KEEPALIVE_INTERVAL_MS)) {
                udp_send_keepalive(g_session_id, CONFIG_GROUP_ID);
                last_ka = xTaskGetTickCount();
                ESP_LOGD(TAG, "udp_tx_task: keepalive sent");
            }
        }

        modem_udp_close();
        total_frames = 0;
        ESP_LOGI(TAG, "udp_tx_task: disconnected, UDP closed");
    }
}
