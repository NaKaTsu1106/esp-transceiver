#include "udp_client.h"
#include "ctrl_client.h"
#include "state_machine.h"
#include "modem.h"
#include "protocol.h"
#include "jitter_buf.h"
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
    // UDPパケット: 8バイトヘッダ + opusペイロード
    // ヘッダに opus_len を含めることで、受信側が連結データグラムを分割できる。
    uint8_t pkt[8 + 128];
    if (opus_len > 128) opus_len = 128;

    udp_header_t hdr;
    proto_build_udp_header(&hdr, session_id, UDP_TYPE_AUDIO, group_id,
                            seq, timestamp, (uint16_t)opus_len);
    memcpy(pkt, &hdr, sizeof(hdr));
    memcpy(pkt + sizeof(hdr), opus_data, opus_len);

    return modem_udp_send(pkt, sizeof(hdr) + opus_len);
}

esp_err_t udp_send_keepalive(uint8_t session_id, uint8_t group_id)
{
    uint8_t pkt[8];
    udp_header_t hdr;
    proto_build_udp_header(&hdr, session_id, UDP_TYPE_KEEPALIVE, group_id, 0, 0, 0);
    memcpy(pkt, &hdr, sizeof(hdr));
    return modem_udp_send(pkt, sizeof(hdr));
}

void udp_rx_task(void *arg)
{
    ESP_LOGI(TAG, "udp_rx_task: started");
    jitter_buf_init();

    while (1) {
        // EVT_CONNECTED が立つまで待機
        xEventGroupWaitBits(g_system_events, EVT_CONNECTED,
                            pdFALSE, pdTRUE, portMAX_DELAY);

        jitter_buf_flush();
        ESP_LOGI(TAG, "udp_rx_task: connected, starting receive loop");

        // モデムが返す最大バイト数（複数パケット連結を含む）
        // SIM7080G は複数 UDP データグラムを連結して返すため、
        // バッファが不足すると末尾パケットが途中で切断され次回受信でヘッダ位置がずれる。
        // 音声パケット 1 つ ≒ 50B × 最大 10 連結 = 500B を余裕を持って収める。
        uint8_t buf[512];

        while (xEventGroupGetBits(g_system_events) & EVT_CONNECTED) {
            size_t got = 0;
            esp_err_t ret = modem_udp_recv(buf, sizeof(buf), &got, 20);

            if (ret != ESP_OK || got == 0) {
                // データなし: AT mutexを解放して ctrl_task / udp_tx が使える時間を確保する。
                // 20ms にすると recv周期が40msになりパケットロス~50%が発生するため5msとする。
                // モデムの応答が ~10ms のため空ポーリング周期 ≈ 15ms < パケット間隔20ms を実現。
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }

            if (got < sizeof(udp_header_t)) {
                ESP_LOGW(TAG, "udp_rx: short packet (%zu B)", got);
                continue;
            }

            // SIM7080G の AT+CARECV は複数 UDP データグラムを連結して返すことがある。
            // ヘッダの opus_len フィールドを用いてバッファ内のパケットを順に分割処理する。
            static uint32_t rx_count = 0;
            size_t offset = 0;
            while (offset + sizeof(udp_header_t) <= got) {
                udp_header_t *hdr = (udp_header_t *)(buf + offset);
                uint8_t type      = proto_udp_type(hdr);
                uint16_t plen     = hdr->opus_len;

                // パケット境界チェック: ヘッダが宣言するペイロードがバッファ内に収まるか確認
                if (offset + sizeof(udp_header_t) + plen > got) {
                    ESP_LOGW(TAG, "udp_rx: truncated pkt at offset=%zu plen=%u got=%zu",
                             offset, plen, got);
                    break;
                }

                offset += sizeof(udp_header_t);

                // キープアライブは無視
                if (type == UDP_TYPE_KEEPALIVE) {
                    offset += plen;
                    continue;
                }

                // LTE RX 試験（session_id=0xFE, サーバー send_test_payload から送信）
                // パターン: [0x00, 0x01, ..., 0x1F] × N。シーケンス欠落も検出する。
                if (hdr->session_id == 0xFE) {
                    static uint16_t rx_next_seq  = 0;
                    static uint32_t rx_pass_count = 0;
                    static uint32_t rx_loss_count = 0;
                    static bool     rx_first      = true;

                    if (rx_first) {
                        rx_next_seq = hdr->seq;
                        rx_first    = false;
                    }

                    /* シーケンス欠落検出 */
                    if (hdr->seq != rx_next_seq) {
                        uint16_t gap = (uint16_t)(hdr->seq - rx_next_seq);
                        rx_loss_count += gap;
                        ESP_LOGW(TAG, "LTE-TEST RX LOSS: expected=%u got=%u gap=%u (total=%u)",
                                 rx_next_seq, hdr->seq, gap, rx_loss_count);
                    }
                    rx_next_seq = (uint16_t)(hdr->seq + 1);

                    /* パターン検証 */
                    const uint8_t *p = buf + offset;
                    uint16_t fail_pos = 0xFFFF;
                    for (uint16_t i = 0; i < plen; i++) {
                        if (p[i] != (uint8_t)(i & 0xFF)) {
                            fail_pos = i;
                            break;
                        }
                    }
                    if (fail_pos == 0xFFFF) {
                        rx_pass_count++;
                        if (rx_pass_count % 50 == 0) {
                            ESP_LOGI(TAG, "LTE-TEST RX PASS: seq=%u pass=%u loss=%u",
                                     hdr->seq, rx_pass_count, rx_loss_count);
                        }
                    } else {
                        ESP_LOGW(TAG, "LTE-TEST RX FAIL: seq=%u byte[%u]=0x%02X expected=0x%02X",
                                 hdr->seq, fail_pos, p[fail_pos], (uint8_t)(fail_pos & 0xFF));
                    }
                    offset += plen;
                    continue;
                }

                // 自分自身のパケットは無視（ループバック防止）
                if (hdr->session_id == g_session_id) {
                    offset += plen;
                    continue;
                }

                // LTE テストエコーフレーム検証
                // magic[0xAA 0x55] + seq(2B LE) + pattern[(i-4)&0xFF × 46B] = 50B
                if (plen >= 4) {
                    const uint8_t *p = buf + offset;
                    if (p[0] == 0xAAu && p[1] == 0x55u) {
                        static uint16_t lte_test_next_seq = 0;
                        static uint32_t lte_test_pass     = 0;
                        static uint32_t lte_test_fail     = 0;
                        static uint32_t lte_test_loss     = 0;
                        static bool     lte_test_first    = true;

                        uint16_t echo_seq = (uint16_t)(p[2] | ((uint16_t)p[3] << 8));

                        if (lte_test_first) {
                            lte_test_next_seq = echo_seq;
                            lte_test_first    = false;
                        }

                        /* シーケンス欠落検出 */
                        if (echo_seq != lte_test_next_seq) {
                            uint16_t gap = (uint16_t)(echo_seq - lte_test_next_seq);
                            lte_test_loss += gap;
                            ESP_LOGW(TAG, "LTE-TEST LOSS: expected=%u got=%u gap=%u (total_loss=%u)",
                                     lte_test_next_seq, echo_seq, gap, lte_test_loss);
                        }
                        lte_test_next_seq = (uint16_t)(echo_seq + 1);

                        /* パターン検証 */
                        uint16_t fail_pos = 0xFFFF;
                        for (uint16_t i = 4; i < plen; i++) {
                            if (p[i] != (uint8_t)((i - 4) & 0xFF)) {
                                fail_pos = i;
                                break;
                            }
                        }
                        if (fail_pos == 0xFFFF) {
                            lte_test_pass++;
                            if (lte_test_pass % 50 == 0) {
                                ESP_LOGI(TAG,
                                         "LTE-TEST PASS: seq=%u pass=%u fail=%u loss=%u",
                                         echo_seq, lte_test_pass,
                                         lte_test_fail, lte_test_loss);
                            }
                        } else {
                            lte_test_fail++;
                            ESP_LOGW(TAG,
                                     "LTE-TEST FAIL: seq=%u byte[%u]=0x%02X expected=0x%02X "
                                     "(pass=%u fail=%u loss=%u)",
                                     echo_seq, fail_pos, p[fail_pos],
                                     (uint8_t)((fail_pos - 4) & 0xFF),
                                     lte_test_pass, lte_test_fail, lte_test_loss);
                        }

                        offset += plen;
                        continue;
                    }
                }

                if (plen == 0) continue;

                // 50パケットごとに診断ログ
                if (rx_count % 50 == 0) {
                    const uint8_t *p = buf + offset;
                    ESP_LOGI(TAG, "udp_rx[%u]: seq=%u got=%zu plen=%u opus[0..1]=%02X %02X",
                             (unsigned)rx_count, hdr->seq, got, plen,
                             plen > 0 ? p[0] : 0xFF,
                             plen > 1 ? p[1] : 0xFF);
                }
                rx_count++;

                jitter_buf_push(buf + offset, plen, hdr->seq);
                ESP_LOGD(TAG, "udp_rx: seq=%u plen=%u", hdr->seq, plen);

                offset += plen;
            }
        }

        jitter_buf_flush();
        ESP_LOGI(TAG, "udp_rx_task: disconnected");
    }
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

        // 開通直後にキープアライブを送信してサーバーに自分の UDP アドレスを登録する。
        // サーバーは受信機から届いた最初のパケットで audio_addr を学習する。
        // これをしないと、PTT を押すまで（最大 25 秒間）サーバーが音声を
        // ドロップし続け、受信機に音声が届かない。
        udp_send_keepalive(g_session_id, CONFIG_GROUP_ID);
        ESP_LOGI(TAG, "udp_tx_task: initial keepalive sent to register audio addr");

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
