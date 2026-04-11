#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "config.h"
#include "axp2101.h"
#include "led.h"
#include "modem.h"
#include "ctrl_client.h"
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

    // Phase 5: VPSサーバー接続（ctrl_taskが担当）
    ESP_LOGI(TAG, "[Phase 5] Server connect (deferred to ctrl_task)");

    // Phase 6: I2S・Opus 音声初期化
    ESP_LOGI(TAG, "[Phase 6] Audio init");
    ESP_ERROR_CHECK(i2s_capture_init());
    ESP_ERROR_CHECK(i2s_playback_init());
    ESP_ERROR_CHECK(opus_codec_init());

    // Phase 7: FreeRTOS タスク起動
    ESP_LOGI(TAG, "[Phase 7] Starting tasks");

    /*
     * コアアサイン方針（doc/ref/main (1).c に準拠）:
     *
     * Core 0 — AT通信 + I2Sハードウェア
     *   I2S DMA 割込みを Core 0 に集約し、Core 1 の Opus 処理と競合させない。
     *   udp_tx (15) > ctrl (5) の優先度差により、音声送信が制御ポーリングを
     *   プリエンプトし、さらに FreeRTOS ミューテックスの priority inheritance で
     *   ctrl がミューテックスを保持中でも udp_tx が速やかに取得できる。
     *
     * Core 1 — 音声処理パイプライン
     *   CPU集約の Opus encode/decode を最高優先で実行。
     *   udp_rx を同コアに置き、受信パイプライン全体（受信→デコード→再生キュー）
     *   のスレッド間受け渡し遅延を最小化する。
     *
     *   Task              Core  Pri  役割
     *   i2s_capture        0    19   INMP441 → pcm_encode_queue
     *   i2s_playback       0    19   pcm_playback_queue → PCM5102
     *   udp_tx             0    15   encoded_tx_queue → AT+CASEND (音声送信)
     *   ctrl               0     5   AT+CARECV/CASEND (制御チャンネル)
     *   heartbeat          0     3   キープアライブ
     *   opus_encode        1    20   PCM → Opus (CPU集約)
     *   opus_decode        1    20   Opus → PCM (CPU集約)
     *   udp_rx             1    18   AT+CARECV → ジッタバッファ (デコードと同コア)
     *   ptt                1     6   GPIO ポーリング
     *   state_machine      1     4   イベント処理
     *   led                1     3   LED 点滅制御
     */

    // Core 0: AT通信 + I2Sハードウェア
    xTaskCreatePinnedToCore(i2s_capture_task,  "i2s_cap",   4096,  NULL, 19, NULL, 0);
    xTaskCreatePinnedToCore(i2s_playback_task, "i2s_play",  4096,  NULL, 19, NULL, 0);
    xTaskCreatePinnedToCore(udp_tx_task,       "udp_tx",    4096,  NULL, 15, NULL, 0);
    xTaskCreatePinnedToCore(ctrl_task,         "ctrl",      8192,  NULL,  5, NULL, 0);
    xTaskCreatePinnedToCore(heartbeat_task,    "heartbeat", 2048,  NULL,  3, NULL, 0);

    // Core 1: 音声処理パイプライン（Opusは内部バッファが大きいため32KBが必要）
    xTaskCreatePinnedToCore(opus_encode_task,  "opus_enc",  32768, NULL, 20, NULL, 1);
    xTaskCreatePinnedToCore(opus_decode_task,  "opus_dec",  32768, NULL, 20, NULL, 1);
    xTaskCreatePinnedToCore(udp_rx_task,       "udp_rx",    4096,  NULL, 18, NULL, 1);
    xTaskCreatePinnedToCore(ptt_task,          "ptt",       2048,  NULL,  6, NULL, 1);
    xTaskCreatePinnedToCore(state_machine_task,"state",     4096,  NULL,  4, NULL, 1);
    xTaskCreatePinnedToCore(led_task,          "led",       2048,  NULL,  3, NULL, 1);

    ESP_LOGI(TAG, "All tasks started.");
}
