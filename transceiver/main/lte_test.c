#include "lte_test.h"
#include "state_machine.h"
#include "config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "lte_test";

/*
 * LTE TX 試験（片方向: デバイス → サーバー）
 *
 * テストパケットフォーマット（50バイト）:
 *   Byte 0   : 0xAA  マジック1
 *   Byte 1   : 0x55  マジック2
 *   Byte 2-3 : seq   リトルエンディアン uint16
 *   Byte 4-49: pattern  (i - 4) & 0xFF  (46バイト)
 *
 * udp_tx_task が AT+CASEND で送信する。受信エコーは行わない。
 * サーバー側ログで受信パケット数・欠落を確認する。
 *
 * LTE RX 試験（片方向: サーバー → デバイス）は
 * サーバーの send_test_payload (session_id=0xFE) を使う。
 * デバイス側の受信検証は udp_rx_task 内の 0xFE ハンドラが担当する。
 */
#define LTE_TEST_MAGIC0      0xAAu
#define LTE_TEST_MAGIC1      0x55u
#define LTE_TEST_PAYLOAD_LEN 50    /* 音声パケット平均サイズ相当 */

static void lte_test_tx_task(void *arg)
{
    ESP_LOGI(TAG, "lte_test_tx_task: waiting for EVT_CONNECTED...");
    xEventGroupWaitBits(g_system_events, EVT_CONNECTED,
                        pdFALSE, pdTRUE, portMAX_DELAY);

    ESP_LOGI(TAG, "lte_test_tx_task: start — %dB/frame @ 20ms (%.1f kbps)",
             LTE_TEST_PAYLOAD_LEN,
             LTE_TEST_PAYLOAD_LEN * 8 / 20.0f);

    uint16_t seq  = 0;
    uint32_t sent = 0;

    while (xEventGroupGetBits(g_system_events) & EVT_CONNECTED) {
        encoded_frame_t f;
        memset(&f, 0, sizeof(f));

        f.data[0] = LTE_TEST_MAGIC0;
        f.data[1] = LTE_TEST_MAGIC1;
        f.data[2] = (uint8_t)(seq & 0xFF);
        f.data[3] = (uint8_t)(seq >> 8);
        for (int i = 4; i < LTE_TEST_PAYLOAD_LEN; i++) {
            f.data[i] = (uint8_t)((i - 4) & 0xFF);
        }
        f.len          = LTE_TEST_PAYLOAD_LEN;
        f.seq          = seq;
        f.timestamp_ms = (uint16_t)((xTaskGetTickCount() * portTICK_PERIOD_MS) & 0xFFFF);

        if (xQueueSend(g_encoded_tx_queue, &f, pdMS_TO_TICKS(1)) == pdTRUE) {
            sent++;
            seq++;
        } else {
            ESP_LOGW(TAG, "tx queue full, dropped seq=%u", seq);
        }

        /* 50フレーム（1秒）ごとに送信カウンタ */
        if (sent > 0 && sent % 50 == 0) {
            ESP_LOGI(TAG, "LTE-TEST TX: sent=%u seq=%u", sent, (unsigned)(seq - 1));
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    ESP_LOGW(TAG, "lte_test_tx_task: disconnected");
    vTaskDelete(NULL);
}

void lte_test_run(void)
{
    ESP_LOGI(TAG, "=== LTE TX TEST MODE ===");
    ESP_LOGI(TAG, "  This tests the TX path only (device → server).");
    ESP_LOGI(TAG, "  Check server log for received packet count and gaps.");
    ESP_LOGI(TAG, "  For RX test: use server send_test_payload via web monitor.");

    xTaskCreatePinnedToCore(lte_test_tx_task, "lte_test_tx", 4096, NULL, 15, NULL, 1);
}
