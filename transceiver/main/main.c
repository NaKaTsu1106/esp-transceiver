#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "config.h"
#include "axp2101.h"
#include "led.h"
#include "modem.h"
#include "tcp_client.h"
#include "udp_client.h"
#include "i2s_capture.h"
#include "i2s_playback.h"
#include "opus_codec.h"
#include "state_machine.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "=== IP Transceiver Boot ===");

    // Phase 1: ESP32-S3 初期化
    ESP_LOGI(TAG, "[Phase 1] System init");
    state_machine_init();

    // Phase 2: AXP2101 PMIC 設定
    ESP_LOGI(TAG, "[Phase 2] PMIC init");
    ESP_ERROR_CHECK(axp2101_init());
    ESP_ERROR_CHECK(led_init());
    led_set(CHGLED_1HZ); // 起動シーケンス中: 1Hz点滅

    // Phase 3: SIM7080G モデム起動
    ESP_LOGI(TAG, "[Phase 3] Modem init");
    ESP_ERROR_CHECK(modem_init());

    // Phase 4: LTE-M ネットワーク接続
    ESP_LOGI(TAG, "[Phase 4] LTE-M connect");
    ESP_ERROR_CHECK(modem_connect());
    state_set(EVT_MODEM_READY);

    // Phase 5: VPSサーバー接続（tcp_taskが担当）
    ESP_LOGI(TAG, "[Phase 5] Server connect (deferred to tcp_task)");

    // Phase 6: I2S・Opus 音声初期化
    ESP_LOGI(TAG, "[Phase 6] Audio init");
    ESP_ERROR_CHECK(i2s_capture_init());
    ESP_ERROR_CHECK(i2s_playback_init());
    ESP_ERROR_CHECK(opus_codec_init());

    // Phase 7: FreeRTOS タスク起動
    ESP_LOGI(TAG, "[Phase 7] Starting tasks");

    // Core 0: 通信系
    xTaskCreatePinnedToCore(tcp_task,       "tcp",       8192, NULL, 8,  NULL, 0);
    xTaskCreatePinnedToCore(udp_rx_task,    "udp_rx",    4096, NULL, 10, NULL, 0);
    xTaskCreatePinnedToCore(udp_tx_task,    "udp_tx",    4096, NULL, 10, NULL, 0);
    xTaskCreatePinnedToCore(heartbeat_task, "heartbeat", 2048, NULL, 5,  NULL, 0);

    // Core 1: 音声系
    xTaskCreatePinnedToCore(opus_encode_task,  "opus_enc",  8192, NULL, 12, NULL, 1);
    xTaskCreatePinnedToCore(opus_decode_task,  "opus_dec",  8192, NULL, 12, NULL, 1);
    xTaskCreatePinnedToCore(i2s_capture_task,  "i2s_cap",   4096, NULL, 15, NULL, 1);
    xTaskCreatePinnedToCore(i2s_playback_task, "i2s_play",  4096, NULL, 15, NULL, 1);
    xTaskCreatePinnedToCore(ptt_task,          "ptt",       2048, NULL, 6,  NULL, 1);
    xTaskCreatePinnedToCore(led_task,          "led",       2048, NULL, 3,  NULL, 1);
    xTaskCreatePinnedToCore(state_machine_task,"state",     4096, NULL, 4,  NULL, 1);

    ESP_LOGI(TAG, "All tasks started.");
}
