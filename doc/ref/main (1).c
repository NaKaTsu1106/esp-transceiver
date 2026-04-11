/*
 * IP Radio PTT — Full Implementation
 * LilyGo T-SIM7080G (ESP32-S3) / ESP-IDF 6.0
 *
 * 仕様書: doc/specification.md
 *
 * menuconfig → "Network Interface" で切り替え:
 *   NET_IFACE_WIFI  : Wi-Fi + lwIP ソケット (ローカルテスト用)
 *   NET_IFACE_MODEM : SIM7080G AT コマンド経由 (本番 LTE 接続)
 *
 * タスク構成 (仕様書 7.5 節):
 *   opus_codec_task   Core 1  pri 20  Opusエンコード/デコード
 *   i2s_capture_task  Core 0  pri 19  I2S → PCMキュー
 *   i2s_playback_task Core 0  pri 19  PCMキュー → I2S
 *   network_tx_task   Core 0  pri 15  RTPキュー → 送信
 *   network_rx_task   Core 0  pri 15  受信 → RTPキュー
 *   control_task      Core 0  pri 10  PTT/状態遷移/HEARTBEAT/CHGLED
 *
 * データフロー:
 *   [I2S RX] → tx_pcm_q → [Opus ENC] → tx_rtp_q → [Net TX]
 *   [Net RX] → rx_rtp_q → [Opus DEC] → rx_pcm_q → [I2S TX]
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <assert.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "opus.h"

#if CONFIG_NET_IFACE_WIFI
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <errno.h>
#else
#include "driver/uart.h"
#endif

static const char *TAG = "ip-radio";

/* =====================================================================
 * オーディオ設定
 * ===================================================================== */
#define SAMPLE_RATE         16000
#define OPUS_FRAME_SAMPLES  320       /* 20ms @ 16kHz */
#define OPUS_BITRATE        24000
#define OPUS_MAX_PKT        256

/* ゲイン設定 (1=等倍, 2/4/8 と倍率指定。飽和クランプあり)
 * MIC_GAIN_X : 送信前のマイク PCM を増幅 → Opus の入力レベルを最大化
 * SPK_GAIN_X : 再生前のデコード PCM を増幅 → スピーカー音量を上げる     */
#define MIC_GAIN_X          4
#define SPK_GAIN_X          2

static inline int16_t gain_clamp(int16_t s, int gain) {
    int32_t v = (int32_t)s * gain;
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return (int16_t)v;
}

#define MIC_BCLK_PIN        CONFIG_MIC_BCLK_PIN
#define MIC_WS_PIN          CONFIG_MIC_WS_PIN
#define MIC_DIN_PIN         CONFIG_MIC_DIN_PIN
#define DAC_BCLK_PIN        CONFIG_DAC_BCLK_PIN
#define DAC_WS_PIN          CONFIG_DAC_WS_PIN
#define DAC_DOUT_PIN        CONFIG_DAC_DOUT_PIN
#define PTT_GPIO            CONFIG_PTT_GPIO

/* =====================================================================
 * ネットワーク設定
 * ===================================================================== */
#define RELAY_SERVER_IP     CONFIG_RELAY_SERVER_IP
#define RELAY_CTRL_PORT     CONFIG_RELAY_CTRL_PORT
#define RELAY_AUDIO_PORT    CONFIG_RELAY_AUDIO_PORT
#define HEARTBEAT_INTERVAL_MS 30000

/* 制御パケットタイプ */
#define TYPE_JOIN       0x01
#define TYPE_LEAVE      0x02
#define TYPE_PTT_ON     0x03
#define TYPE_PTT_OFF    0x04
#define TYPE_HEARTBEAT  0x05
#define TYPE_ACK        0x06
#define TYPE_PTT_DENY   0x07
#define ACK_OK          0x00

/* RTP */
#define RTP_HDR_LEN     12
#define RTP_PT_OPUS     111
#define RTP_VERSION     2

/* =====================================================================
 * AXP2101 / PMU 定義
 * ===================================================================== */
#define PMU_I2C_PORT        I2C_NUM_0
#define PMU_SDA_PIN         15
#define PMU_SCL_PIN         7
#define PMU_I2C_FREQ        400000
#define AXP2101_ADDR        0x34

/* AXP2101 レジスタ */
#define AXP_DC_CTRL0        0x80
#define AXP_DC3_VOL         0x84
#define AXP_BLDO_CTRL       0x90
#define AXP_BLDO1_VOL       0x96
#define AXP_CHGLED_REG      0x69
#define AXP_BLDO1_BIT       (1 << 4)
#define AXP_DC3_BIT         (1 << 2)

/* CHGLED モード (bits[5:4]=11 手動, bits[2:0]=パターン) */
#define CHGLED_MASK         0x37
#define CHGLED_MANUAL       0x30
#define CHGLED_OFF          (CHGLED_MANUAL | 0x00)
#define CHGLED_1HZ          (CHGLED_MANUAL | 0x01)
#define CHGLED_2HZ          (CHGLED_MANUAL | 0x02)
#define CHGLED_4HZ          (CHGLED_MANUAL | 0x03)
#define CHGLED_ON           (CHGLED_MANUAL | 0x05)

/* =====================================================================
 * データ型
 * ===================================================================== */

/* PTT 状態 */
typedef enum {
    PTT_INIT,       /* 起動・接続中  CHGLED: 4Hz */
    PTT_IDLE,       /* 待機中        CHGLED: 常時点灯 */
    PTT_TX,         /* 送信中        CHGLED: 4Hz */
    PTT_TX_ENDING,  /* 送信終了中    CHGLED: 2Hz */
    PTT_RX,         /* 受信中        CHGLED: 2Hz */
} ptt_state_t;

/* PCM フレーム (640 bytes) */
typedef struct { int16_t s[OPUS_FRAME_SAMPLES]; } pcm_frame_t;

/* RTP パケット */
typedef struct {
    uint8_t  buf[RTP_HDR_LEN + OPUS_MAX_PKT];
    uint16_t len;
} rtp_pkt_t;

/* =====================================================================
 * グローバル状態
 * ===================================================================== */
static volatile ptt_state_t s_ptt_state = PTT_INIT;
static SemaphoreHandle_t     s_state_mutex;

static QueueHandle_t s_tx_pcm_q;   /* I2S capture → Opus enc */
static QueueHandle_t s_tx_rtp_q;   /* Opus enc → Net TX       */
static QueueHandle_t s_rx_rtp_q;   /* Net RX  → Opus dec      */
static QueueHandle_t s_rx_pcm_q;   /* Opus dec → I2S playback */

static i2s_chan_handle_t s_i2s_rx;
static i2s_chan_handle_t s_i2s_tx;

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_axp_dev;

static uint32_t s_ssrc;

/* RX タイムアウト用: 最後に RTP を受信した tick (network_rx_task が更新) */
#define RX_AUDIO_TIMEOUT_MS  3000   /* この期間 RTP が来なければ RX 終了とみなす */
static volatile TickType_t s_last_rtp_rx_tick = 0;

/* ジッターバッファ: RX 開始時に N フレーム溜まるまでデコードを遅延 */
#define JITTER_BUF_FRAMES    25     /* 10 frames = 200ms */
static volatile bool s_rx_buffering = false;

/* =====================================================================
 * AXP2101 ユーティリティ
 * ===================================================================== */
static esp_err_t axp_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_axp_dev, buf, 2, pdMS_TO_TICKS(100));
}

static esp_err_t axp_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_axp_dev, &reg, 1, val, 1,
                                       pdMS_TO_TICKS(100));
}

static void axp2101_set_chgled(uint8_t mode)
{
    uint8_t v = 0;
    axp_read(AXP_CHGLED_REG, &v);
    v = (v & ~CHGLED_MASK) | (mode & CHGLED_MASK);
    axp_write(AXP_CHGLED_REG, v);
}

#if !CONFIG_NET_IFACE_WIFI
static void axp2101_power_modem(void)
{
    /* BLDO1: 3300mV — UART レベル変換 IC 電源 */
    axp_write(AXP_BLDO1_VOL, (3300 - 500) / 100);   /* (3300-500)/100 = 0x1C */
    uint8_t v = 0;
    axp_read(AXP_BLDO_CTRL, &v);
    axp_write(AXP_BLDO_CTRL, v | AXP_BLDO1_BIT);

    /* DCDC3: 3000mV — モデム主電源 (冷起動時は一旦 OFF → ON) */
    axp_read(AXP_DC_CTRL0, &v);
    if (v & AXP_DC3_BIT) {
        axp_write(AXP_DC_CTRL0, v & ~AXP_DC3_BIT);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    axp_write(AXP_DC3_VOL, (3000 - 1600) / 100 + 0x58);   /* 0x66 */
    axp_read(AXP_DC_CTRL0, &v);
    axp_write(AXP_DC_CTRL0, v | AXP_DC3_BIT);
    ESP_LOGI(TAG, "AXP2101: BLDO1=3300mV DCDC3=3000mV");
}
#endif /* !CONFIG_NET_IFACE_WIFI */

/* =====================================================================
 * 共通: SSRC 生成 (eFuse MAC ベース)
 * ===================================================================== */
static uint32_t get_ssrc(void)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BASE);
    return ((uint32_t)mac[2] << 24) | ((uint32_t)mac[3] << 16) |
           ((uint32_t)mac[4] <<  8) |  (uint32_t)mac[5];
}

/* =====================================================================
 * 共通: 制御パケット組み立て
 * ===================================================================== */
static void build_ctrl_pkt(uint8_t *buf, uint8_t type, uint8_t group_id,
                            uint32_t ssrc)
{
    buf[0] = type;
    buf[1] = group_id;
    buf[2] = (ssrc >> 24) & 0xFF;
    buf[3] = (ssrc >> 16) & 0xFF;
    buf[4] = (ssrc >>  8) & 0xFF;
    buf[5] = (ssrc      ) & 0xFF;
    buf[6] = 0x00;
    buf[7] = 0x00;
}

static void build_join_pkt(uint8_t *buf, uint32_t ssrc, uint16_t audio_port)
{
    build_ctrl_pkt(buf, TYPE_JOIN, 0x00, ssrc);
    buf[8] = (audio_port >> 8) & 0xFF;
    buf[9] = (audio_port     ) & 0xFF;
}

/* =====================================================================
 * 共通: RTP パケット組み立て
 * ===================================================================== */
static void build_rtp_hdr(uint8_t *hdr, bool marker,
                           uint16_t seq, uint32_t ts, uint32_t ssrc)
{
    hdr[0] = (RTP_VERSION << 6);
    hdr[1] = (marker ? 0x80 : 0x00) | (RTP_PT_OPUS & 0x7F);
    hdr[2] = (seq >> 8) & 0xFF;
    hdr[3] = (seq     ) & 0xFF;
    hdr[4] = (ts >> 24) & 0xFF;
    hdr[5] = (ts >> 16) & 0xFF;
    hdr[6] = (ts >>  8) & 0xFF;
    hdr[7] = (ts      ) & 0xFF;
    hdr[8] = (ssrc >> 24) & 0xFF;
    hdr[9] = (ssrc >> 16) & 0xFF;
    hdr[10]= (ssrc >>  8) & 0xFF;
    hdr[11]= (ssrc      ) & 0xFF;
}

static bool parse_rtp_hdr(const uint8_t *buf, int len,
                           uint16_t *seq, uint32_t *ts, uint32_t *ssrc)
{
    if (len < RTP_HDR_LEN) return false;
    if (((buf[0] >> 6) & 0x03) != RTP_VERSION) return false;
    *seq  = ((uint16_t)buf[2] << 8) | buf[3];
    *ts   = ((uint32_t)buf[4] << 24) | ((uint32_t)buf[5] << 16) |
            ((uint32_t)buf[6] <<  8) |  (uint32_t)buf[7];
    *ssrc = ((uint32_t)buf[8] << 24) | ((uint32_t)buf[9] << 16) |
            ((uint32_t)buf[10] << 8) |  (uint32_t)buf[11];
    return true;
}

/* =====================================================================
 * PTT 状態アクセス (スレッドセーフ)
 * ===================================================================== */
static ptt_state_t state_get(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    ptt_state_t s = s_ptt_state;
    xSemaphoreGive(s_state_mutex);
    return s;
}

static void state_set(ptt_state_t next)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_ptt_state = next;
    xSemaphoreGive(s_state_mutex);
}

/* =====================================================================
 * Wi-Fi パス
 * ===================================================================== */
#if CONFIG_NET_IFACE_WIFI

#define WIFI_RETRY_MAX 5
static EventGroupHandle_t s_wifi_eg;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry = 0;

static void wifi_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry < WIFI_RETRY_MAX) {
            esp_wifi_connect();
            s_retry++;
            ESP_LOGW(TAG, "Wi-Fi retry %d/%d", s_retry, WIFI_RETRY_MAX);
        } else {
            xEventGroupSetBits(s_wifi_eg, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "Wi-Fi IP: " IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry = 0;
        xEventGroupSetBits(s_wifi_eg, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_connect(void)
{
    s_wifi_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t h_wifi, h_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler, NULL, &h_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_handler, NULL, &h_ip));

    wifi_config_t wc = {};
    strlcpy((char *)wc.sta.ssid,     CONFIG_WIFI_SSID,     sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, CONFIG_WIFI_PASSWORD, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
    ESP_LOGI(TAG, "Connecting: %s", CONFIG_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_eg,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
        pdMS_TO_TICKS(30000));

    esp_event_handler_instance_unregister(IP_EVENT,   IP_EVENT_STA_GOT_IP, h_ip);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,    h_wifi);
    vEventGroupDelete(s_wifi_eg);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

/* 制御ソケット (JOIN/PTT_ON/PTT_OFF/HEARTBEAT) */
static int s_ctrl_sock = -1;
/* RTP 送信ソケット */
static int s_rtp_tx_sock = -1;
/* RTP 受信ソケット (RELAY_AUDIO_PORT にバインド) */
static int s_rtp_rx_sock = -1;

static struct sockaddr_in s_relay_ctrl_addr;
static struct sockaddr_in s_relay_rtp_addr;

static bool net_init_wifi(void)
{
    /* 共通: サーバーアドレス設定 */
    s_relay_ctrl_addr.sin_family = AF_INET;
    s_relay_ctrl_addr.sin_port   = htons(RELAY_CTRL_PORT);
    if (!inet_aton(RELAY_SERVER_IP, &s_relay_ctrl_addr.sin_addr)) {
        ESP_LOGE(TAG, "Bad relay IP: %s", RELAY_SERVER_IP);
        return false;
    }
    s_relay_rtp_addr = s_relay_ctrl_addr;
    s_relay_rtp_addr.sin_port = htons(RELAY_AUDIO_PORT);

    /* 制御ソケット */
    s_ctrl_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_ctrl_sock < 0) return false;

    /* RTP 送信ソケット */
    s_rtp_tx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_rtp_tx_sock < 0) return false;

    /* RTP 受信ソケット (ローカルポートにバインド) */
    s_rtp_rx_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_rtp_rx_sock < 0) return false;
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port   = htons(RELAY_AUDIO_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(s_rtp_rx_sock, (struct sockaddr *)&local, sizeof(local)) < 0) {
        ESP_LOGE(TAG, "RTP bind failed: errno=%d", errno);
        return false;
    }
    /* 受信タイムアウト 25ms (1フレーム相当) */
    struct timeval tv = { .tv_sec = 0, .tv_usec = 25000 };
    setsockopt(s_rtp_rx_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    return true;
}

static bool relay_join_wifi(void)
{
    struct timeval tv = { .tv_sec = 5 };
    setsockopt(s_ctrl_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t pkt[10];
    build_join_pkt(pkt, s_ssrc, RELAY_AUDIO_PORT);
    int r = sendto(s_ctrl_sock, pkt, sizeof(pkt), 0,
                   (struct sockaddr *)&s_relay_ctrl_addr,
                   sizeof(s_relay_ctrl_addr));
    if (r < 0) { ESP_LOGE(TAG, "JOIN send: errno=%d", errno); return false; }
    ESP_LOGI(TAG, "JOIN → %s:%d  ssrc=0x%08" PRIX32,
             RELAY_SERVER_IP, RELAY_CTRL_PORT, s_ssrc);

    uint8_t ack[16];
    int n = recv(s_ctrl_sock, ack, sizeof(ack), 0);
    struct timeval tv0 = {};
    setsockopt(s_ctrl_sock, SOL_SOCKET, SO_RCVTIMEO, &tv0, sizeof(tv0));

    if (n < 9 || ack[0] != TYPE_ACK || ack[8] != ACK_OK) {
        ESP_LOGE(TAG, "JOIN ACK failed (n=%d)", n);
        return false;
    }
    ESP_LOGI(TAG, "JOIN ACK OK  group=%d", ack[1]);
    return true;
}

/* 制御パケット送信 (PTT_ON / PTT_OFF / HEARTBEAT) */
static void net_send_ctrl_wifi(uint8_t type)
{
    uint8_t pkt[8];
    build_ctrl_pkt(pkt, type, 0x00, s_ssrc);
    sendto(s_ctrl_sock, pkt, sizeof(pkt), 0,
           (struct sockaddr *)&s_relay_ctrl_addr, sizeof(s_relay_ctrl_addr));
}

/* 制御パケット受信 (非ブロッキング, 25ms タイムアウト) */
static int net_recv_ctrl_wifi(uint8_t *buf, int maxlen)
{
    struct timeval tv = { .tv_sec = 0, .tv_usec = 25000 };
    setsockopt(s_ctrl_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int n = recv(s_ctrl_sock, buf, maxlen, 0);
    struct timeval tv0 = {};
    setsockopt(s_ctrl_sock, SOL_SOCKET, SO_RCVTIMEO, &tv0, sizeof(tv0));
    return n;
}

/* RTP 送信 */
static void net_send_rtp_wifi(const rtp_pkt_t *pkt)
{
    sendto(s_rtp_tx_sock, pkt->buf, pkt->len, 0,
           (struct sockaddr *)&s_relay_rtp_addr, sizeof(s_relay_rtp_addr));
}

/* RTP 受信 (25ms タイムアウト) */
static int net_recv_rtp_wifi(uint8_t *buf, int maxlen)
{
    return recv(s_rtp_rx_sock, buf, maxlen, 0);
}

/* HEARTBEAT ACK 受信確認 */
static void heartbeat_ack_wifi(void)
{
    uint8_t ack[16];
    struct timeval tv = { .tv_sec = 3 };
    setsockopt(s_ctrl_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int n = recv(s_ctrl_sock, ack, sizeof(ack), 0);
    struct timeval tv0 = {};
    setsockopt(s_ctrl_sock, SOL_SOCKET, SO_RCVTIMEO, &tv0, sizeof(tv0));
    if (n >= 9 && ack[0] == TYPE_ACK && ack[8] == ACK_OK)
        ESP_LOGI(TAG, "HEARTBEAT ACK OK");
    else
        ESP_LOGW(TAG, "HEARTBEAT no ACK (n=%d)", n);
}

/* =====================================================================
 * モデムパス (SIM7080G AT コマンド)
 * ===================================================================== */
#else /* CONFIG_NET_IFACE_MODEM */

#define MODEM_UART_PORT  UART_NUM_1
#define MODEM_TX_PIN     5
#define MODEM_RX_PIN     4
#define MODEM_BAUD       115200
#define MODEM_BAUD_HIGH  921600
#define MODEM_BUF_SIZE   4096
#define MODEM_PWRKEY_PIN 41
#define MODEM_DTR_PIN    42

#define SORACOM_APN  "soracom.io"
#define SORACOM_USER "sora"
#define SORACOM_PASS "sora"

static SemaphoreHandle_t s_at_mutex;

static bool at_send(const char *cmd, const char *expect, uint32_t timeout_ms)
{
    uart_flush_input(MODEM_UART_PORT);
    if (cmd && cmd[0]) {
        char line[256];
        snprintf(line, sizeof(line), "%s\r\n", cmd);
        uart_write_bytes(MODEM_UART_PORT, line, strlen(line));
        ESP_LOGD(TAG, ">> %s", cmd);
    }
    static char resp[4096];
    int total = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (xTaskGetTickCount() < deadline) {
        int n = uart_read_bytes(MODEM_UART_PORT,
                                (uint8_t *)(resp + total),
                                sizeof(resp) - total - 1,
                                pdMS_TO_TICKS(50));
        if (n > 0) {
            total += n;
            resp[total] = '\0';
            if (strstr(resp, expect)) return true;
            if (strstr(resp, "+CME ERROR") || strstr(resp, "\r\nERROR\r\n"))
                return false;
        }
    }
    return false;
}

/* AT+CASEND でバイナリデータを送信 (制御パケット用: OK応答を確認する) */
static bool at_send_binary(int cid, const uint8_t *data, int len)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CASEND=%d,%d", cid, len);
    if (!at_send(cmd, ">", 3000)) return false;
    uart_write_bytes(MODEM_UART_PORT, (const char *)data, len);
    static char rbuf[64];
    int total = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(200);
    while (xTaskGetTickCount() < deadline) {
        int n = uart_read_bytes(MODEM_UART_PORT, (uint8_t *)(rbuf + total),
                                sizeof(rbuf) - total - 1, pdMS_TO_TICKS(10));
        if (n > 0) {
            total += n;
            rbuf[total] = '\0';
            if (strstr(rbuf, "OK") || strstr(rbuf, "SEND OK")) {
                ESP_LOGD(TAG, "CASEND: OK");
                return true;
            }
            if (strstr(rbuf, "ERROR")) {
                for (int k = 0; k < total; k++) if (rbuf[k] < 0x20) rbuf[k] = ' ';
                ESP_LOGE(TAG, "CASEND error: %s", rbuf);
                return false;
            }
        }
    }
    ESP_LOGW(TAG, "CASEND: no response (cid=%d len=%d)", cid, len);
    return true;
}

/* AT+CASEND でRTPパケットを高速送信 (UDP なので OK 応答を待たない)
 * uart_flush_input を呼ばないことで RX バッファを保護する */
static void at_send_rtp_fast(const uint8_t *data, int len)
{
    char cmd[32];
    int cmdlen = snprintf(cmd, sizeof(cmd), "AT+CASEND=1,%d\r\n", len);
    uart_write_bytes(MODEM_UART_PORT, cmd, cmdlen);

    /* '>' プロンプト待ち (flush なし、短いタイムアウト) */
    static char pbuf[16];
    int total = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(50);
    while (xTaskGetTickCount() < deadline) {
        int n = uart_read_bytes(MODEM_UART_PORT, (uint8_t *)(pbuf + total),
                                sizeof(pbuf) - total - 1, pdMS_TO_TICKS(5));
        if (n > 0) {
            total += n;
            pbuf[total] = '\0';
            if (strchr(pbuf, '>')) break;
        }
    }
    uart_write_bytes(MODEM_UART_PORT, (const char *)data, len);
    /* OK 応答は待たない: UDP なので到着確認不要、次フレームを優先 */
}

/* AT+CARECV でデータを読み取る。戻り値: 受信バイト数 (0=データなし/-1=エラー) */
static int at_recv_binary(int cid, uint8_t *buf, int maxlen)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CARECV=%d,%d", cid, maxlen);
    uart_flush_input(MODEM_UART_PORT);
    char line[64];
    snprintf(line, sizeof(line), "%s\r\n", cmd);
    uart_write_bytes(MODEM_UART_PORT, line, strlen(line));

    /* +CARECV: length,<binary>\r\nOK を読み取る */
    static char resp[4096];
    int total = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(500);
    while (xTaskGetTickCount() < deadline) {
        int n = uart_read_bytes(MODEM_UART_PORT,
                                (uint8_t *)(resp + total),
                                sizeof(resp) - total - 1,
                                pdMS_TO_TICKS(50));
        if (n > 0) {
            total += n;
            resp[total] = '\0';
            if (strstr(resp, "\r\nOK")) break;
            if (strstr(resp, "ERROR")) return -1;
        }
    }
    /* "+CARECV: N," を探してデータ先頭を特定 */
    char *p = strstr(resp, "+CARECV: ");
    if (!p) return 0;
    int dlen = 0;
    char *comma = strchr(p + 9, ',');
    if (!comma) return 0;
    dlen = atoi(p + 9);
    if (dlen <= 0) return 0;
    if (dlen > maxlen) dlen = maxlen;
    memcpy(buf, comma + 1, dlen);
    return dlen;
}

static void modem_power_on(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << MODEM_PWRKEY_PIN) | (1ULL << MODEM_DTR_PIN),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(MODEM_DTR_PIN,    0);
    gpio_set_level(MODEM_PWRKEY_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(MODEM_PWRKEY_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level(MODEM_PWRKEY_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(2000));
    ESP_LOGI(TAG, "PWRKEY done");
}

static bool modem_init(void)
{
    ESP_LOGI(TAG, "modem_init: start");

    /* 起動時はモデムOFFが基本なので最初に PWRKEY を送出する */
    ESP_LOGI(TAG, "Sending initial PWRKEY...");
    modem_power_on();

    /* ブートメッセージを監視しながら AT 応答を待つ
     * 高速ボーレード (NVRAM 設定) を優先して試す */
    bool ready = false;
    const int try_bauds[] = { MODEM_BAUD_HIGH, MODEM_BAUD };
    for (int cycle = 0; cycle < 3 && !ready; cycle++) {
        if (cycle > 0) {
            ESP_LOGW(TAG, "Retry PWRKEY (cycle %d/3)...", cycle + 1);
            modem_power_on();
        }

        /* SIM7080G 起動後にブートメッセージを監視 (最大 2s、RDY検出で早期終了)
         * modem_power_on() がすでに PWRKEY 後 2s 待つため長く待たない */
        ESP_LOGI(TAG, "Listening for boot messages (max 2s)...");
        TickType_t boot_end = xTaskGetTickCount() + pdMS_TO_TICKS(2000);
        while (xTaskGetTickCount() < boot_end) {
            uint8_t raw[64] = {0};
            int raw_n = uart_read_bytes(MODEM_UART_PORT, raw, sizeof(raw) - 1,
                                        pdMS_TO_TICKS(200));
            if (raw_n > 0) {
                int printable = 0;
                for (int k = 0; k < raw_n; k++)
                    if (raw[k] >= 0x20 && raw[k] < 0x7F) printable++;
                if (printable * 2 >= raw_n) {
                    raw[raw_n] = '\0';
                    for (int k = 0; k < raw_n; k++)
                        if (raw[k] < 0x20) raw[k] = '.';
                    ESP_LOGI(TAG, "  [boot] ASCII: %s", (char *)raw);
                    /* RDY を受信したら即座に AT 試行へ */
                    if (strstr((char *)raw, "RDY")) break;
                } else {
                    char hex[128] = {0};
                    int hlen = 0;
                    for (int k = 0; k < raw_n && hlen < (int)sizeof(hex) - 4; k++)
                        hlen += snprintf(hex + hlen, sizeof(hex) - hlen, "%02X ", raw[k]);
                    ESP_LOGI(TAG, "  [boot] hex (wrong baud?): %s", hex);
                }
            }
        }

        /* AT 応答確認: 高速優先で両ボーレードを試す */
        for (int b = 0; b < 2 && !ready; b++) {
            uart_set_baudrate(MODEM_UART_PORT, try_bauds[b]);
            ESP_LOGI(TAG, "Trying AT at %d baud...", try_bauds[b]);
            for (int i = 0; i < 10 && !ready; i++) {
                uart_flush_input(MODEM_UART_PORT);
                if (at_send("AT", "OK", 1000)) {
                    ESP_LOGI(TAG, "Modem ready at %d baud (cycle=%d attempt=%d)",
                             try_bauds[b], cycle + 1, i + 1);
                    ready = true;
                } else {
                    vTaskDelay(pdMS_TO_TICKS(300));
                }
            }
        }
        if (!ready) uart_set_baudrate(MODEM_UART_PORT, MODEM_BAUD_HIGH);
    }
    if (!ready) { ESP_LOGE(TAG, "modem_init: no AT response"); return false; }

    /* AT 安定確認: 連続 3 回 OK を受け取れるまで待つ */
    ESP_LOGI(TAG, "Confirming modem stability...");
    int ok_count = 0;
    for (int i = 0; i < 10 && ok_count < 3; i++) {
        if (at_send("AT", "OK", 500)) {
            ok_count++;
            ESP_LOGD(TAG, "  AT OK %d/3", ok_count);
        } else {
            ok_count = 0;
            ESP_LOGW(TAG, "  AT failed, resetting count (attempt %d)", i + 1);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
    if (ok_count < 3) { ESP_LOGE(TAG, "modem_init: modem unstable"); return false; }
    ESP_LOGI(TAG, "Modem stable");

    ESP_LOGI(TAG, "Sending init commands...");
    at_send("ATE0",      "OK", 1000);
    at_send("AT+CMEE=2", "OK", 1000);

    ESP_LOGI(TAG, "Checking SIM...");
    bool sim_ready = false;
    for (int i = 0; i < 5 && !sim_ready; i++) {
        if (at_send("AT+CPIN?", "READY", 3000)) {
            sim_ready = true;
            ESP_LOGI(TAG, "SIM ready (attempt %d)", i + 1);
        } else {
            ESP_LOGW(TAG, "SIM not ready yet (%d/5)", i + 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    if (!sim_ready) { ESP_LOGE(TAG, "SIM not ready"); return false; }

    bool csq_ok = at_send("AT+CSQ", "OK", 2000);
    ESP_LOGI(TAG, "CSQ: %s", csq_ok ? "OK" : "failed");

    ESP_LOGI(TAG, "Setting LTE Cat-M1...");
    at_send("AT+CFUN=0", "OK", 10000);
    at_send("AT+CMNB=3", "OK",  5000);
    at_send("AT+CBANDCFG=\"CAT-M\",1,3,8,18,19,26,28",  "OK", 5000);
    at_send("AT+CBANDCFG=\"NB-IOT\",1,3,8,18,19,26,28", "OK", 5000);
    at_send("AT+CFUN=1", "OK", 10000);
    /* SORACOM SIM: JP KDDI (44051) へ手動 PLMN 指定 */
    ESP_LOGI(TAG, "Setting PLMN to JP KDDI (44051)...");
    at_send("AT+COPS=1,2,\"44051\",7", "OK", 60000);
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "modem_init: complete");
    axp2101_set_chgled(CHGLED_1HZ);  /* LTE 登録待ち */
    return true;
}

static void modem_set_baud_high(void)
{
    uint32_t current_baud = 0;
    uart_get_baudrate(MODEM_UART_PORT, &current_baud);
    if (current_baud == MODEM_BAUD_HIGH) {
        ESP_LOGI(TAG, "Modem already at %d baud", MODEM_BAUD_HIGH);
        return;
    }
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+IPR=%d", MODEM_BAUD_HIGH);
    at_send(cmd, "OK", 2000);
    uart_set_baudrate(MODEM_UART_PORT, MODEM_BAUD_HIGH);
    vTaskDelay(pdMS_TO_TICKS(100));
    if (at_send("AT", "OK", 1000)) {
        ESP_LOGI(TAG, "Modem baud switched to %d", MODEM_BAUD_HIGH);
    } else {
        ESP_LOGE(TAG, "Baud switch failed, falling back to %d", MODEM_BAUD);
        uart_set_baudrate(MODEM_UART_PORT, MODEM_BAUD);
    }
}

static bool soracom_connect(void)
{
    char cmd[128];
    bool ok;

    ESP_LOGI(TAG, "soracom_connect: configuring APN...");
    snprintf(cmd, sizeof(cmd), "AT+CGDCONT=1,\"IP\",\"%s\"", SORACOM_APN);
    ok = at_send(cmd, "OK", 5000);
    ESP_LOGI(TAG, "  CGDCONT: %s", ok ? "OK" : "failed");

    snprintf(cmd, sizeof(cmd), "AT+CNCFG=0,1,\"%s\",\"%s\",\"%s\",1",
             SORACOM_APN, SORACOM_USER, SORACOM_PASS);
    ok = at_send(cmd, "OK", 5000);
    ESP_LOGI(TAG, "  CNCFG: %s", ok ? "OK" : "failed");

    /* 登録前の診断情報 (at_send_log: コマンドとレスポンスをログ出力) */
    {
        /* レスポンスを読み出して表示するローカルヘルパー */
        #define AT_LOG(cmd, timeout) do { \
            uart_flush_input(MODEM_UART_PORT); \
            uart_write_bytes(MODEM_UART_PORT, cmd "\r\n", sizeof(cmd) + 1); \
            char _r[256] = {0}; \
            uart_read_bytes(MODEM_UART_PORT, (uint8_t *)_r, sizeof(_r)-1, pdMS_TO_TICKS(timeout)); \
            for (int _k = 0; _r[_k]; _k++) if (_r[_k]=='\r'||_r[_k]=='\n') _r[_k]=' '; \
            ESP_LOGI(TAG, "  >> " cmd " => %s", _r); \
        } while(0)

        AT_LOG("AT+CSQ",   2000);  /* 電波強度: 99,99=圏外 */
        AT_LOG("AT+COPS?", 5000);  /* 現在のオペレーター */
        AT_LOG("AT+CMNB?", 2000);  /* ネットワークモード */
        #undef AT_LOG
    }

    ESP_LOGI(TAG, "Waiting for LTE registration (max 120s)...");
    for (int i = 0; i < 60; i++) {
        char resp[256] = {};
        uart_flush_input(MODEM_UART_PORT);
        uart_write_bytes(MODEM_UART_PORT, "AT+CEREG?\r\n", 11);
        vTaskDelay(pdMS_TO_TICKS(200));
        uart_read_bytes(MODEM_UART_PORT, (uint8_t *)resp, sizeof(resp)-1,
                        pdMS_TO_TICKS(1000));
        resp[sizeof(resp)-1] = '\0';

        /* 改行を除いて1行にして表示 */
        for (int k = 0; resp[k]; k++) if (resp[k] == '\r' || resp[k] == '\n') resp[k] = ' ';
        ESP_LOGI(TAG, "  CEREG[%2d/60]: %s", i + 1, resp);

        /* stat=1: home, stat=5: roaming */
        if (strstr(resp, ",1") || strstr(resp, ",5")) {
            ESP_LOGI(TAG, "LTE registered");
            ESP_LOGI(TAG, "Activating PDP context...");
            ok = at_send("AT+CNACT=0,1", "OK", 15000);
            ESP_LOGI(TAG, "  CNACT: %s", ok ? "OK" : "failed");
            vTaskDelay(pdMS_TO_TICKS(2000));
            at_send("AT+CNACT?", "OK", 3000);
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGE(TAG, "soracom_connect: LTE registration timeout");
    return false;
}

static bool relay_join_modem(void)
{
    char cmd[128];

    /* ネットワーク疎通確認 */
    {
        char ping_cmd[64];
        snprintf(ping_cmd, sizeof(ping_cmd),
                 "AT+SNPING4=\"%s\",2,32,5000", RELAY_SERVER_IP);
        ESP_LOGI(TAG, "Ping to %s...", RELAY_SERVER_IP);
        bool ping_ok = at_send(ping_cmd, "OK", 20000);
        ESP_LOGI(TAG, "Ping: %s", ping_ok ? "OK" : "FAILED (no route?)");
    }

    /* 既存ソケットをクローズ (前回の残骸対策) */
    at_send("AT+CACLOSE=0", "OK", 3000);
    at_send("AT+CACLOSE=1", "OK", 3000);

    /* cid=0: 制御チャネル */
    snprintf(cmd, sizeof(cmd), "AT+CAOPEN=0,0,\"UDP\",\"%s\",%d",
             RELAY_SERVER_IP, RELAY_CTRL_PORT);
    ESP_LOGI(TAG, "CAOPEN ctrl: %s", cmd);
    if (!at_send(cmd, "OK", 15000)) { ESP_LOGE(TAG, "CAOPEN ctrl failed"); return false; }
    ESP_LOGI(TAG, "CAOPEN ctrl: OK");

    /* connectID=1: RTP チャネル */
    snprintf(cmd, sizeof(cmd), "AT+CAOPEN=1,0,\"UDP\",\"%s\",%d",
             RELAY_SERVER_IP, RELAY_AUDIO_PORT);
    ESP_LOGI(TAG, "CAOPEN rtp: %s", cmd);
    if (!at_send(cmd, "OK", 15000)) { ESP_LOGE(TAG, "CAOPEN rtp failed"); return false; }
    ESP_LOGI(TAG, "CAOPEN rtp: OK");

    /* JOIN パケット内容をログ */
    uint8_t pkt[10];
    build_join_pkt(pkt, s_ssrc, RELAY_AUDIO_PORT);
    ESP_LOGI(TAG, "JOIN pkt: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
             pkt[0],pkt[1],pkt[2],pkt[3],pkt[4],pkt[5],pkt[6],pkt[7],pkt[8],pkt[9]);
    bool sent = at_send_binary(0, pkt, sizeof(pkt));
    ESP_LOGI(TAG, "JOIN send: %s", sent ? "OK" : "FAILED");
    if (!sent) return false;
    ESP_LOGI(TAG, "JOIN → %s:%d  ssrc=0x%08" PRIX32,
             RELAY_SERVER_IP, RELAY_CTRL_PORT, s_ssrc);

    /* LTE 往復遅延を考慮して ACK を最大 3000ms ポーリング */
    uint8_t ack[32];
    int n = 0;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
    while (xTaskGetTickCount() < deadline) {
        n = at_recv_binary(0, ack, sizeof(ack));
        if (n >= 9 && ack[0] == TYPE_ACK) break;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (n < 9 || ack[0] != TYPE_ACK || ack[8] != ACK_OK) {
        ESP_LOGE(TAG, "JOIN ACK failed (n=%d)", n);
        return false;
    }
    ESP_LOGI(TAG, "JOIN ACK OK  group=%d", ack[1]);
    return true;
}

static void net_send_ctrl_modem(uint8_t type)
{
    xSemaphoreTake(s_at_mutex, portMAX_DELAY);
    uint8_t pkt[8];
    build_ctrl_pkt(pkt, type, 0x00, s_ssrc);
    at_send_binary(0, pkt, sizeof(pkt));
    xSemaphoreGive(s_at_mutex);
}

static int net_recv_ctrl_modem(uint8_t *buf, int maxlen)
{
    xSemaphoreTake(s_at_mutex, portMAX_DELAY);
    int n = at_recv_binary(0, buf, maxlen);
    xSemaphoreGive(s_at_mutex);
    return n;
}

static void net_send_rtp_modem(const rtp_pkt_t *pkt)
{
    xSemaphoreTake(s_at_mutex, portMAX_DELAY);
    at_send_rtp_fast(pkt->buf, pkt->len);
    xSemaphoreGive(s_at_mutex);
}

static int net_recv_rtp_modem(uint8_t *buf, int maxlen)
{
    xSemaphoreTake(s_at_mutex, portMAX_DELAY);
    int n = at_recv_binary(1, buf, maxlen);
    xSemaphoreGive(s_at_mutex);
    return n;
}

static void heartbeat_ack_modem(void)
{
    /* LTE 往復遅延は最大 ~1000ms になることがある。余裕を持って 1500ms ポーリング */
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(1500);
    while (xTaskGetTickCount() < deadline) {
        xSemaphoreTake(s_at_mutex, portMAX_DELAY);
        uint8_t ack[16];
        int n = at_recv_binary(0, ack, sizeof(ack));
        xSemaphoreGive(s_at_mutex);
        if (n >= 9 && ack[0] == TYPE_ACK && ack[8] == ACK_OK) {
            ESP_LOGI(TAG, "HEARTBEAT ACK OK");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGW(TAG, "HEARTBEAT no ACK");
}

#endif /* NET_IFACE_WIFI / NET_IFACE_MODEM */

/* =====================================================================
 * ネットワーク抽象レイヤー (インターフェース共通ラッパー)
 * ===================================================================== */
static void net_send_ctrl(uint8_t type)
{
#if CONFIG_NET_IFACE_WIFI
    net_send_ctrl_wifi(type);
#else
    net_send_ctrl_modem(type);
#endif
}

static int net_recv_ctrl(uint8_t *buf, int maxlen)
{
#if CONFIG_NET_IFACE_WIFI
    return net_recv_ctrl_wifi(buf, maxlen);
#else
    return net_recv_ctrl_modem(buf, maxlen);
#endif
}

static void net_send_rtp(const rtp_pkt_t *pkt)
{
#if CONFIG_NET_IFACE_WIFI
    net_send_rtp_wifi(pkt);
#else
    net_send_rtp_modem(pkt);
#endif
}

static int net_recv_rtp(uint8_t *buf, int maxlen)
{
#if CONFIG_NET_IFACE_WIFI
    return net_recv_rtp_wifi(buf, maxlen);
#else
    return net_recv_rtp_modem(buf, maxlen);
#endif
}

static void heartbeat_ack(void)
{
#if CONFIG_NET_IFACE_WIFI
    heartbeat_ack_wifi();
#else
    heartbeat_ack_modem();
#endif
}

/* =====================================================================
 * I2S 初期化
 * ===================================================================== */
static void i2s_init(void)
{
    /* RX: INMP441 マイク (ステレオ受信、L ch のみ使用) */
    i2s_chan_config_t rx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&rx_cfg, NULL, &s_i2s_rx));
    i2s_std_config_t rx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = MIC_BCLK_PIN, .ws = MIC_WS_PIN,
            .dout = I2S_GPIO_UNUSED, .din = MIC_DIN_PIN,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_rx, &rx_std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_i2s_rx));

    /* TX: PCM5102A DAC */
    i2s_chan_config_t tx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&tx_cfg, &s_i2s_tx, NULL));
    i2s_std_config_t tx_std = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = DAC_BCLK_PIN, .ws = DAC_WS_PIN,
            .dout = DAC_DOUT_PIN, .din = I2S_GPIO_UNUSED,
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_i2s_tx, &tx_std));
    ESP_ERROR_CHECK(i2s_channel_enable(s_i2s_tx));

    ESP_LOGI(TAG, "I2S RX: MIC BCLK=%d WS=%d DIN=%d",
             MIC_BCLK_PIN, MIC_WS_PIN, MIC_DIN_PIN);
    ESP_LOGI(TAG, "I2S TX: DAC BCLK=%d WS=%d DOUT=%d",
             DAC_BCLK_PIN, DAC_WS_PIN, DAC_DOUT_PIN);
}

/* =====================================================================
 * I2S キャプチャタスク (Core 0, pri 19)
 * ===================================================================== */
static void i2s_capture_task(void *arg)
{
    static int32_t raw[OPUS_FRAME_SAMPLES * 2];  /* L/R インターリーブ */
    size_t bytes_read;

    while (1) {
        i2s_channel_read(s_i2s_rx, raw, sizeof(raw), &bytes_read, portMAX_DELAY);
        int pairs = bytes_read / (2 * sizeof(int32_t));

        /* TX 状態のときのみキューに積む */
        if (state_get() == PTT_TX) {
            pcm_frame_t frame;
            for (int i = 0; i < pairs && i < OPUS_FRAME_SAMPLES; i++)
                frame.s[i] = gain_clamp((int16_t)(raw[i * 2] >> 16), MIC_GAIN_X);
            xQueueSend(s_tx_pcm_q, &frame, 0);  /* あふれたら捨てる */
        }
    }
}

/* =====================================================================
 * I2S 再生タスク (Core 0, pri 19)
 * ===================================================================== */
static void i2s_playback_task(void *arg)
{
    static int32_t tx_buf[OPUS_FRAME_SAMPLES * 2];
    size_t bytes_written;

    while (1) {
        pcm_frame_t frame;
        if (xQueueReceive(s_rx_pcm_q, &frame, pdMS_TO_TICKS(25)) == pdTRUE) {
            for (int i = 0; i < OPUS_FRAME_SAMPLES; i++) {
                int32_t s = (int32_t)gain_clamp(frame.s[i], SPK_GAIN_X) << 16;
                tx_buf[i * 2]     = s;
                tx_buf[i * 2 + 1] = s;
            }
        } else {
            /* データなし: DMA バッファの残留音声が繰り返されないよう無音で上書き */
            memset(tx_buf, 0, sizeof(tx_buf));
        }
        i2s_channel_write(s_i2s_tx, tx_buf, sizeof(tx_buf),
                          &bytes_written, portMAX_DELAY);
    }
}

/* =====================================================================
 * Opus コーデックタスク (Core 1, pri 20)
 * TX: tx_pcm_q → Opus encode → tx_rtp_q
 * RX: rx_rtp_q → Opus decode → rx_pcm_q
 * ===================================================================== */
static void opus_codec_task(void *arg)
{
    int err;
    OpusEncoder *enc = opus_encoder_create(SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
    assert(err == OPUS_OK && enc);
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(0));
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(OPUS_BITRATE));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(enc, OPUS_SET_DTX(1));
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(10));

    OpusDecoder *dec = opus_decoder_create(SAMPLE_RATE, 1, &err);
    assert(err == OPUS_OK && dec);

    ESP_LOGI(TAG, "Opus ready: %dHz mono %dbps complexity=0 DTX+FEC",
             SAMPLE_RATE, OPUS_BITRATE);

    static uint8_t enc_buf[OPUS_MAX_PKT];
    uint16_t rtp_seq = 0;
    uint32_t rtp_ts  = 0;
    bool     first_frame = true;

    while (1) {
        vTaskDelay(1);  /* IDLE1 が WDT をリセットできるよう 1tick 譲る */
        ptt_state_t st = state_get();

        if (st == PTT_TX) {
            pcm_frame_t frame;
            if (xQueueReceive(s_tx_pcm_q, &frame, pdMS_TO_TICKS(5)) != pdTRUE)
                continue;

            int enc_len = opus_encode(enc, frame.s, OPUS_FRAME_SAMPLES,
                                      enc_buf, sizeof(enc_buf));
            if (enc_len < 0) {
                ESP_LOGW(TAG, "opus_encode: %s", opus_strerror(enc_len));
                continue;
            }

            rtp_pkt_t pkt;
            build_rtp_hdr(pkt.buf, first_frame, rtp_seq, rtp_ts, s_ssrc);
            memcpy(pkt.buf + RTP_HDR_LEN, enc_buf, enc_len);
            pkt.len = RTP_HDR_LEN + enc_len;
            /* pdMS_TO_TICKS(40): 送信タスクが 2 フレーム分遅れるまで待つ。
             * 0 だとキュー満杯時に無音ドロップが発生する。 */
            xQueueSend(s_tx_rtp_q, &pkt, pdMS_TO_TICKS(40));

            rtp_seq++;
            rtp_ts += OPUS_FRAME_SAMPLES;
            first_frame = false;

        } else if (st == PTT_RX) {
            /* ジッターバッファ: JITTER_BUF_FRAMES 溜まるまでデコードを遅延 */
            if (s_rx_buffering) {
                UBaseType_t q = uxQueueMessagesWaiting(s_rx_rtp_q);
                if (q < JITTER_BUF_FRAMES) {
                    vTaskDelay(pdMS_TO_TICKS(5));
                    continue;
                }
                s_rx_buffering = false;
                ESP_LOGI(TAG, "RX jitter buffer ready (%u frames)", (unsigned)q);
            }

            rtp_pkt_t rtp;
            if (xQueueReceive(s_rx_rtp_q, &rtp, pdMS_TO_TICKS(5)) != pdTRUE) {
                /* パケットロス: PLC で補完 */
                pcm_frame_t out;
                int plc = opus_decode(dec, NULL, 0, out.s, OPUS_FRAME_SAMPLES, 0);
                /* portMAX_DELAY で待機: 再生タスクのペース(20ms/frame)に追従させる。
                 * timeout=0 だとキュー満杯時にフレームが捨てられ音途切れの原因になる。 */
                if (plc > 0) xQueueSend(s_rx_pcm_q, &out, portMAX_DELAY);
                continue;
            }
            pcm_frame_t out;
            const uint8_t *payload = rtp.buf + RTP_HDR_LEN;
            int plen = rtp.len - RTP_HDR_LEN;
            int n = opus_decode(dec, payload, plen, out.s, OPUS_FRAME_SAMPLES, 0);
            if (n > 0)
                xQueueSend(s_rx_pcm_q, &out, portMAX_DELAY);
        } else {
            /* TX でも RX でもない場合は RTP タイムスタンプをリセット準備 */
            first_frame = true;
        }
    }
}

/* =====================================================================
 * ネットワーク TX タスク (Core 0, pri 15)
 * ===================================================================== */
static void network_tx_task(void *arg)
{
    rtp_pkt_t pkt;
    while (1) {
        if (xQueueReceive(s_tx_rtp_q, &pkt, portMAX_DELAY) == pdTRUE)
            net_send_rtp(&pkt);
    }
}

/* =====================================================================
 * ネットワーク RX タスク (Core 0, pri 15)
 * ===================================================================== */
static void network_rx_task(void *arg)
{
    static uint8_t buf[RTP_HDR_LEN + OPUS_MAX_PKT];
    while (1) {
        /* RX 状態のときのみ積極的にポーリング、それ以外は 25ms スリープ */
        if (state_get() != PTT_RX) {
            vTaskDelay(pdMS_TO_TICKS(25));
            continue;
        }
        int n = net_recv_rtp(buf, sizeof(buf));
        if (n < RTP_HDR_LEN) continue;

        uint16_t seq; uint32_t ts, ssrc;
        if (!parse_rtp_hdr(buf, n, &seq, &ts, &ssrc)) continue;

        s_last_rtp_rx_tick = xTaskGetTickCount();  /* RX タイムアウト計測用 */

        rtp_pkt_t pkt;
        pkt.len = n;
        memcpy(pkt.buf, buf, n);
        xQueueSend(s_rx_rtp_q, &pkt, 0);
    }
}

/* =====================================================================
 * コントロールタスク (Core 0, pri 10)
 * PTT ボタン / 状態遷移 / HEARTBEAT / CHGLED 更新
 * ===================================================================== */
static void control_task(void *arg)
{
    /* PTT ボタン GPIO 設定 */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PTT_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    TickType_t hb_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS);
    bool ptt_prev = false;  /* 前回のボタン状態 */

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(20));  /* 20ms ポーリング */

        ptt_state_t st = state_get();

        /* ---------- CHGLED 更新 ---------- */
        switch (st) {
        case PTT_INIT:       axp2101_set_chgled(CHGLED_4HZ); break;
        case PTT_IDLE:       axp2101_set_chgled(CHGLED_ON);  break;
        case PTT_TX:         axp2101_set_chgled(CHGLED_4HZ); break;
        case PTT_TX_ENDING:  axp2101_set_chgled(CHGLED_2HZ); break;
        case PTT_RX:         axp2101_set_chgled(CHGLED_2HZ); break;
        }

        /* ---------- HEARTBEAT ----------
         * IDLE と RX 中は送信する (TX 中は音声優先で省略)。
         * RX 中も 30 秒間隔で送ることでリレーのタイムアウト除去 (60s) を防ぐ。
         * ------------------------------------------------------------------ */
        if (xTaskGetTickCount() >= hb_deadline) {
            if (st == PTT_IDLE || st == PTT_RX) {
                net_send_ctrl(TYPE_HEARTBEAT);
                ESP_LOGI(TAG, "HEARTBEAT sent (state=%d)", (int)st);
                if (st == PTT_IDLE) heartbeat_ack();  /* RX 中は ACK 待ちをスキップ */
            }
            hb_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS);
        }

        /* ---------- PTT ボタン ---------- */
        bool ptt_now = (gpio_get_level(PTT_GPIO) == 0);

        if (ptt_now && !ptt_prev) {
            /* 押下エッジ: IDLE → TX 試行 */
            if (st == PTT_IDLE) {
                ESP_LOGI(TAG, "PTT press → PTT_ON");
                net_send_ctrl(TYPE_PTT_ON);
                /* ACK 待ち (最大 500ms) */
                uint8_t ack[16];
                TickType_t t = xTaskGetTickCount();
                int n;
                do {
                    n = net_recv_ctrl(ack, sizeof(ack));
                    if (n >= 9 && ack[0] == TYPE_ACK && ack[8] == ACK_OK) {
                        state_set(PTT_TX);
                        ESP_LOGI(TAG, "TX start");
                        break;
                    }
                    if (n >= 9 && ack[0] == TYPE_PTT_DENY) {
                        ESP_LOGW(TAG, "PTT denied");
                        break;
                    }
                } while (xTaskGetTickCount() - t < pdMS_TO_TICKS(500));
            }
        } else if (!ptt_now && ptt_prev) {
            /* 解放エッジ: TX → TX_ENDING → IDLE */
            if (st == PTT_TX) {
                state_set(PTT_TX_ENDING);
                /* 送信キューが空になるまで待って PTT_OFF 送信 (最大 200ms) */
                {
                    TickType_t drain_end = xTaskGetTickCount() + pdMS_TO_TICKS(200);
                    while (uxQueueMessagesWaiting(s_tx_rtp_q) > 0 &&
                           xTaskGetTickCount() < drain_end) {
                        vTaskDelay(pdMS_TO_TICKS(5));
                    }
                }
                net_send_ctrl(TYPE_PTT_OFF);
                state_set(PTT_IDLE);
                ESP_LOGI(TAG, "TX end → IDLE");
            }
        }

        /* ---------- 着信制御パケット処理 ----------
         * st ではなく state_get() で現在状態を確認する。
         * PTT_ON ACK 受信後に st がキャッシュ値 (IDLE) のまま残っていても
         * PTT_TX に上書きされるのを防ぐため。
         * ------------------------------------------------------------ */
        if (state_get() == PTT_IDLE) {
            uint8_t ctrl[16];
            int n = net_recv_ctrl(ctrl, sizeof(ctrl));
            if (n >= 6 && ctrl[0] == TYPE_PTT_ON) {
                /* 自分の PTT_ON エコーは無視 (リレーが送信者にも転送するため) */
                uint32_t pkt_ssrc = ((uint32_t)ctrl[2] << 24) |
                                    ((uint32_t)ctrl[3] << 16) |
                                    ((uint32_t)ctrl[4] <<  8) |
                                     (uint32_t)ctrl[5];
                if (pkt_ssrc != s_ssrc) {
                    s_last_rtp_rx_tick = xTaskGetTickCount();
                    s_rx_buffering = true;  /* デコード前にバッファを蓄積する */
                    state_set(PTT_RX);
                    ESP_LOGI(TAG, "RX start (ssrc=0x%08" PRIX32 ") buffering...", pkt_ssrc);
                }
            }
        }

        /* ---------- RX 終了検出 ----------
         * (a) PTT_OFF 受信: 正常終了
         * (b) RTP 無音タイムアウト: PTT_OFF が UDP ロストした場合のフォールバック
         * ------------------------------------------------------------------ */
        if (state_get() == PTT_RX) {
            bool rx_end = false;

            /* (a) 制御パケットを読み取り PTT_OFF を確認 */
            uint8_t ctrl[16];
            int n = net_recv_ctrl(ctrl, sizeof(ctrl));
            if (n >= 1) {
                if (ctrl[0] == TYPE_PTT_OFF) {
                    ESP_LOGI(TAG, "RX end: PTT_OFF received");
                    rx_end = true;
                } else {
                    ESP_LOGD(TAG, "RX: ctrl pkt type=0x%02X ignored", ctrl[0]);
                }
            }

            /* (b) RTP が一定時間来ていなければタイムアウト終了 */
            if (!rx_end && s_last_rtp_rx_tick != 0) {
                TickType_t silent = xTaskGetTickCount() - s_last_rtp_rx_tick;
                if (silent > pdMS_TO_TICKS(RX_AUDIO_TIMEOUT_MS)) {
                    ESP_LOGW(TAG, "RX end: audio timeout (%lu ms)",
                             (unsigned long)pdTICKS_TO_MS(silent));
                    rx_end = true;
                }
            }

            if (rx_end) {
                s_last_rtp_rx_tick = 0;
                s_rx_buffering = false;
                /* 先に IDLE へ遷移して opus_codec_task の PLC 生成を止める。
                 * ここより前に state_set しないと opus_codec_task が PLC を
                 * 生成し続けキューが永久に空にならず WDT が発火する。        */
                state_set(PTT_IDLE);
                ESP_LOGI(TAG, "RX end → IDLE");
                /* 残存フレームを再生し切るまで最大 200ms 待つ */
                TickType_t drain_end = xTaskGetTickCount() + pdMS_TO_TICKS(200);
                while (uxQueueMessagesWaiting(s_rx_pcm_q) > 0 &&
                       xTaskGetTickCount() < drain_end) {
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
            }
        }

        ptt_prev = ptt_now;
    }
}

/* =====================================================================
 * app_main
 * ===================================================================== */
void app_main(void)
{
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  IP Radio PTT  [%s mode]",
#if CONFIG_NET_IFACE_WIFI
             "Wi-Fi"
#else
             "Modem"
#endif
             );
    ESP_LOGI(TAG, "  Relay: %s ctrl=%d audio=%d",
             RELAY_SERVER_IP, RELAY_CTRL_PORT, RELAY_AUDIO_PORT);
    ESP_LOGI(TAG, "========================================");

    /* I2C バス (AXP2101 PMU) */
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port          = PMU_I2C_PORT,
        .sda_io_num        = PMU_SDA_PIN,
        .scl_io_num        = PMU_SCL_PIN,
        .clk_source        = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = AXP2101_ADDR,
        .scl_speed_hz    = PMU_I2C_FREQ,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_axp_dev));

    /* 起動 LED: 4Hz 点滅 */
    axp2101_set_chgled(CHGLED_4HZ);

    /* SSRC */
    s_ssrc = get_ssrc();
    ESP_LOGI(TAG, "SSRC = 0x%08" PRIX32, s_ssrc);

    /* ミューテックス */
    s_state_mutex = xSemaphoreCreateMutex();
    assert(s_state_mutex);

    /* キュー作成 */
    s_tx_pcm_q = xQueueCreate(5,                       sizeof(pcm_frame_t));
    s_tx_rtp_q = xQueueCreate(5,                       sizeof(rtp_pkt_t));
    s_rx_rtp_q = xQueueCreate(JITTER_BUF_FRAMES + 10, sizeof(rtp_pkt_t));  /* バッファ+余裕 */
    s_rx_pcm_q = xQueueCreate(5,                       sizeof(pcm_frame_t));
    assert(s_tx_pcm_q && s_tx_rtp_q && s_rx_rtp_q && s_rx_pcm_q);

    /* ネットワーク接続 */
#if CONFIG_NET_IFACE_WIFI
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    if (!wifi_connect()) { ESP_LOGE(TAG, "Wi-Fi failed"); axp2101_set_chgled(CHGLED_OFF); return; }
    if (!net_init_wifi()) { ESP_LOGE(TAG, "Net init failed"); axp2101_set_chgled(CHGLED_OFF); return; }
    if (!relay_join_wifi()) { ESP_LOGE(TAG, "JOIN failed"); axp2101_set_chgled(CHGLED_OFF); return; }
#else
    s_at_mutex = xSemaphoreCreateMutex();
    assert(s_at_mutex);

    axp2101_power_modem();
    vTaskDelay(pdMS_TO_TICKS(500));

    /* UART 初期化 */
    uart_config_t uart_cfg = {
        .baud_rate  = MODEM_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(MODEM_UART_PORT, MODEM_BUF_SIZE, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(MODEM_UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(MODEM_UART_PORT, MODEM_TX_PIN, MODEM_RX_PIN,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    if (!modem_init())      { ESP_LOGE(TAG, "Modem init failed");    axp2101_set_chgled(CHGLED_OFF); return; }
    modem_set_baud_high();
    if (!soracom_connect()) { ESP_LOGE(TAG, "SORACOM connect failed"); axp2101_set_chgled(CHGLED_OFF); return; }
    if (!relay_join_modem()) { ESP_LOGE(TAG, "JOIN failed");          axp2101_set_chgled(CHGLED_OFF); return; }
#endif

    /* IDLE へ遷移 */
    state_set(PTT_IDLE);
    axp2101_set_chgled(CHGLED_ON);
    ESP_LOGI(TAG, "Connected. Ready.");

    /* I2S 初期化 */
    i2s_init();

    /* タスク起動 */
    /*
     * コアアサイン方針:
     *   Core 1: 受信パイプライン (network_rx → rx_rtp_q → opus_codec → rx_pcm_q)
     *           RTP受信とOpusデコードを同一コアに集約し、Core 0 の I2S 割込みと競合させない
     *   Core 0: I2S ハードウェア / 送信 / 制御
     *           i2s_playback が DMA 割込みを専有できる
     *
     *   Task          Core  Pri  役割
     *   opus_codec     1    20   Opusエンコード/デコード (CPU集約)
     *   network_rx     1    18   RTP受信 → rx_rtp_q (デコードと同コアで受け渡し高速化)
     *   i2s_capture    0    19   I2S マイク → tx_pcm_q
     *   i2s_playback   0    19   rx_pcm_q → I2S DAC
     *   network_tx     0    15   tx_rtp_q → 送信
     *   control        0    10   PTT / HEARTBEAT / CHGLED
     */
    xTaskCreatePinnedToCore(opus_codec_task,    "opus",     32768, NULL, 20, NULL, 1);
    xTaskCreatePinnedToCore(network_rx_task,    "net_rx",   8192,  NULL, 18, NULL, 1);
    xTaskCreatePinnedToCore(i2s_capture_task,   "i2s_cap",  4096,  NULL, 19, NULL, 0);
    xTaskCreatePinnedToCore(i2s_playback_task,  "i2s_play", 4096,  NULL, 19, NULL, 0);
    xTaskCreatePinnedToCore(network_tx_task,    "net_tx",   8192,  NULL, 15, NULL, 0);
    xTaskCreatePinnedToCore(control_task,       "ctrl",     4096,  NULL, 10, NULL, 0);
}
