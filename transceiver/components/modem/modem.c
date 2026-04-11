#include "modem.h"
#include "at_cmd.h"
#include "led.h"
#include "config.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "modem";

#define MODEM_RETRY_MAX  3
#define CTRL_CONN_ID     0   // 制御チャンネル UDP コネクションID
#define UDP_CONN_ID      1   // 音声チャンネル UDP コネクションID

// ATコマンド排他ミューテックス（tcp_task からも使用）
static SemaphoreHandle_t s_at_mutex = NULL;

// ---------- 内部: GPIO初期化 ----------

/*
 * PWRKEY / DTR を OUTPUT LOW に初期化する。
 * DCDC3 電源投入より前または直後に必ず呼ぶこと。
 * GPIO41 が INPUT のまま浮遊すると、トランジスタ経由で PWRKEY が不定アサートされ
 * モデムが正常にブートできない。
 *
 * トランジスタ反転: GPIO HIGH → PWRKEY LOW（アサート）
 *                   GPIO LOW  → PWRKEY HIGH（解放）
 */
static void gpio_init_modem(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << CONFIG_MODEM_PWR_PIN) | (1ULL << CONFIG_MODEM_DTR_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(CONFIG_MODEM_PWR_PIN, 0);    // LOW = トランジスタ OFF = PWRKEY 解放
    gpio_set_level(CONFIG_MODEM_DTR_PIN,  0);    // LOW = 通信モード
    ESP_LOGI(TAG, "GPIO init: PWRKEY=GPIO%d DTR=GPIO%d",
             CONFIG_MODEM_PWR_PIN, CONFIG_MODEM_DTR_PIN);
}

// ---------- 内部: ボーレートネゴシエーション ----------

static esp_err_t baud_negotiate(void)
{
    // 3-3: 921600 bps で疎通確認
    at_cmd_init(CONFIG_MODEM_BAUD_RATE);
    if (at_cmd_send("AT", "OK", 3000) == ESP_OK) {
        ESP_LOGI(TAG, "Modem ready at %lu bps", CONFIG_MODEM_BAUD_RATE);
        return ESP_OK;
    }

    // 3-4: 115200 bps にフォールバック（最大 MODEM_RETRY_MAX 回）
    ESP_LOGW(TAG, "921600 bps failed, trying fallback %lu bps", CONFIG_MODEM_BAUD_FALLBACK);
    for (int i = 0; i < MODEM_RETRY_MAX; i++) {
        at_cmd_set_baud(CONFIG_MODEM_BAUD_FALLBACK);
        if (at_cmd_send("AT", "OK", 5000) == ESP_OK) {
            // 3-5: 921600 bps へアップグレード
            ESP_LOGI(TAG, "Modem at fallback baud, upgrading...");
            at_cmd_send("AT+IPR=921600", "OK", 3000);   // 失敗しても続行
            at_cmd_set_baud(CONFIG_MODEM_BAUD_RATE);
            vTaskDelay(pdMS_TO_TICKS(100));
            if (at_cmd_send("AT", "OK", 2000) == ESP_OK) {
                ESP_LOGI(TAG, "Upgraded to %lu bps", CONFIG_MODEM_BAUD_RATE);
            } else {
                // アップグレード失敗 → fallback のまま続行
                at_cmd_set_baud(CONFIG_MODEM_BAUD_FALLBACK);
                ESP_LOGW(TAG, "Baud upgrade failed, continuing at %lu bps",
                         CONFIG_MODEM_BAUD_FALLBACK);
            }
            return ESP_OK;
        }
        ESP_LOGW(TAG, "Fallback retry %d/%d", i + 1, MODEM_RETRY_MAX);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGE(TAG, "Modem not responding");
    return ESP_ERR_TIMEOUT;
}

// ---------- 外部 API ----------

esp_err_t modem_init(void)
{
    esp_err_t ret;

    // ミューテックス初期化
    if (s_at_mutex == NULL) {
        s_at_mutex = xSemaphoreCreateMutex();
        configASSERT(s_at_mutex);
    }

    // GPIO 初期化
    gpio_init_modem();

    // すでに AT が通る場合はパワーサイクルをスキップ（2重トグル防止）
    at_cmd_init(CONFIG_MODEM_BAUD_RATE);
    if (at_cmd_send("AT", "OK", 1000) != ESP_OK) {
        // 3-1〜3-2: PWRKEY パルス（トランジスタ反転: GPIO HIGH → PWRKEY LOW アサート）
        // LOW(100ms 解放) → HIGH(1000ms アサート ≥1.3s でトグル) → LOW(解放)
        ESP_LOGI(TAG, "Powering on modem...");
        gpio_set_level(CONFIG_MODEM_PWR_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(CONFIG_MODEM_PWR_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
        gpio_set_level(CONFIG_MODEM_PWR_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(2000));    // モデム起動待ち
        ESP_LOGI(TAG, "PWRKEY pulse done");
    } else {
        ESP_LOGI(TAG, "Modem already running");
    }

    // 3-3〜3-5: ボーレートネゴシエーション
    ret = baud_negotiate();
    if (ret != ESP_OK) return ret;

    // 3-10: エコーバック無効（失敗しても続行）
    at_cmd_send("ATE0", "OK", 2000);

    // 3-11: PSM（Power Saving Mode）無効化
    // LTE-M モジュールはデフォルトで PSM が有効になっており、
    // アイドル時に T3324 タイマー満了後 LTE から切断して深睡眠に入る。
    // 復帰時は LTE 再登録が発生し RDY/+CFUN:/+CPIN: 等の URC が出る。
    // PTT ラジオは常時受信待機が必要なため PSM は無効にする。
    if (at_cmd_send("AT+CPSMS=0", "OK", 3000) == ESP_OK) {
        ESP_LOGI(TAG, "PSM disabled");
    } else {
        ESP_LOGW(TAG, "PSM disable failed (may not be supported)");
    }

    // 3-12: eDRX（Extended Discontinuous Reception）無効化
    // eDRX は受信間隔を数秒〜数十秒に延ばす省電力機能。
    // 有効だと着信パケットの遅延が増大するため無効化する。
    if (at_cmd_send("AT+CEDRXS=0", "OK", 3000) == ESP_OK) {
        ESP_LOGI(TAG, "eDRX disabled");
    } else {
        ESP_LOGW(TAG, "eDRX disable failed (may not be supported)");
    }

    // 3-6: SIM認識確認（失敗時はLED消灯で停止、再起動しない）
    bool sim_ready = false;
    for (int i = 0; i < MODEM_RETRY_MAX; i++) {
        if (at_cmd_send("AT+CPIN?", "READY", 5000) == ESP_OK) {
            sim_ready = true;
            break;
        }
        ESP_LOGW(TAG, "SIM check retry %d/%d", i + 1, MODEM_RETRY_MAX);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!sim_ready) {
        ESP_LOGE(TAG, "SIM not found - insert SIM and reboot");
        led_set(CHGLED_OFF);
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }  // ユーザー対応待ち（再起動しない）
    }
    ESP_LOGI(TAG, "SIM ready");

    // 3-7: 通信規格を LTE-M のみに設定
    for (int i = 0; i < MODEM_RETRY_MAX; i++) {
        if (at_cmd_send("AT+CNMP=38", "OK", 5000) == ESP_OK &&
            at_cmd_send("AT+CMNB=1",  "OK", 5000) == ESP_OK) break;
        ESP_LOGW(TAG, "LTE-M mode set retry %d", i + 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "LTE-M only mode set");

    // 3-8: APN 設定
    for (int i = 0; i < MODEM_RETRY_MAX; i++) {
        if (at_cmd_send("AT+CGDCONT=1,\"IP\",\"soracom.io\"", "OK", 5000) == ESP_OK) break;
        ESP_LOGW(TAG, "APN set retry %d", i + 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "APN set: soracom.io");

    // 3-9: 認証設定（PAP）
    for (int i = 0; i < MODEM_RETRY_MAX; i++) {
        if (at_cmd_send("AT+CGAUTH=1,1,\"sora\",\"sora\"", "OK", 5000) == ESP_OK) break;
        ESP_LOGW(TAG, "Auth set retry %d", i + 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(TAG, "Auth set (PAP)");

    ESP_LOGI(TAG, "modem_init OK");
    return ESP_OK;
}

esp_err_t modem_connect(void)
{
    // 4-1: ネットワーク登録待機（最大 3 回 × 60秒）
    bool registered = false;
    for (int attempt = 0; attempt < 3 && !registered; attempt++) {
        TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(60000);
        while (xTaskGetTickCount() < deadline) {
            char resp[128];
            if (at_cmd_query("AT+CEREG?", resp, sizeof(resp), 5000) == ESP_OK) {
                if (strstr(resp, "+CEREG: 0,1") || strstr(resp, "+CEREG: 0,5")) {
                    registered = true;
                    break;
                }
            }
            ESP_LOGW(TAG, "Waiting for LTE-M registration...");
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
        if (!registered) {
            ESP_LOGW(TAG, "Registration timeout, retry %d/3", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(30000));
        }
    }
    if (!registered) {
        ESP_LOGE(TAG, "LTE-M registration failed");
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "LTE-M registered");

    // 4-2: PDP コンテキスト有効化
    for (int i = 0; i < MODEM_RETRY_MAX; i++) {
        if (at_cmd_send("AT+CGACT=1,1", "OK", 10000) == ESP_OK) break;
        ESP_LOGW(TAG, "PDP activate retry %d", i + 1);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGI(TAG, "PDP context activated");

    // 4-2b: アプリケーションネットワーク設定 + 有効化
    // SIM7080G の TCP Application Toolkit (CAOPEN) は CNCFG/CNACT で管理する
    // アプリネットワーク 0 に APN を設定
    at_cmd_send("AT+CNCFG=0,1,\"soracom.io\",\"sora\",\"sora\",1", "OK", 5000);
    for (int i = 0; i < MODEM_RETRY_MAX; i++) {
        if (at_cmd_send("AT+CNACT=0,1", "OK", 10000) == ESP_OK) break;
        ESP_LOGW(TAG, "CNACT retry %d", i + 1);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    vTaskDelay(pdMS_TO_TICKS(1000));   // アプリネット確立待ち
    ESP_LOGI(TAG, "App network activated");

    // 4-3: IP アドレス取得確認
    char ip_buf[32] = {0};
    for (int i = 0; i < MODEM_RETRY_MAX; i++) {
        if (modem_get_ip(ip_buf, sizeof(ip_buf)) == ESP_OK) break;
        ESP_LOGW(TAG, "IP addr retry %d", i + 1);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    ESP_LOGI(TAG, "IP address: %s", ip_buf);

    // 4-4: 電波強度確認（ログのみ）
    char csq_resp[64];
    if (at_cmd_query("AT+CSQ", csq_resp, sizeof(csq_resp), 3000) == ESP_OK) {
        // "+CSQ: 27,99" から数値部分だけ抽出
        char *p = strstr(csq_resp, "+CSQ: ");
        if (p) ESP_LOGI(TAG, "Signal (CSQ): %.*s", 6, p + 6);
    }

    // 4-5: PSM 状態確認（ログのみ。+CPSMS: 0 であれば無効化済み）
    char psm_resp[64];
    if (at_cmd_query("AT+CPSMS?", psm_resp, sizeof(psm_resp), 3000) == ESP_OK) {
        ESP_LOGI(TAG, "PSM status: %s", strstr(psm_resp, "+CPSMS:") ? strstr(psm_resp, "+CPSMS:") : psm_resp);
    }

    ESP_LOGI(TAG, "modem_connect OK");
    return ESP_OK;
}

esp_err_t modem_get_ip(char *ip_buf, size_t len)
{
    char resp[128];
    esp_err_t ret = at_cmd_query("AT+CGPADDR=1", resp, sizeof(resp), 5000);
    if (ret != ESP_OK) return ret;

    // "+CGPADDR: 1,10.x.x.x" — カンマの直後がIPアドレス
    char *p = strstr(resp, "+CGPADDR:");
    if (!p) return ESP_ERR_NOT_FOUND;
    p = strchr(p, ',');
    if (!p) return ESP_ERR_NOT_FOUND;
    p++;    // カンマをスキップ

    // 改行・CR・スペースまでがIPアドレス
    size_t ip_len = 0;
    while (p[ip_len] && p[ip_len] != '\r' && p[ip_len] != '\n' &&
           p[ip_len] != ' ' && ip_len < len - 1) {
        ip_len++;
    }
    if (ip_len == 0) return ESP_ERR_NOT_FOUND;

    memcpy(ip_buf, p, ip_len);
    ip_buf[ip_len] = '\0';
    return ESP_OK;
}

// ---------- ネットワーク復旧 API ----------

esp_err_t modem_ensure_network(void)
{
    xSemaphoreTake(s_at_mutex, portMAX_DELAY);
    ESP_LOGI(TAG, "modem_ensure_network: re-activating network...");

    // モデムが再起動していた場合、エコーバックが ON に戻っているので再度 OFF にする
    at_cmd_send("ATE0", "OK", 2000);

    // LTE 登録確認（最大 30 秒待つ）
    // モデム自発リセット後は LTE 再登録が必要な場合がある
    bool registered = false;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(30000);
    while (xTaskGetTickCount() < deadline) {
        char resp[128];
        if (at_cmd_query("AT+CEREG?", resp, sizeof(resp), 5000) == ESP_OK) {
            if (strstr(resp, "+CEREG: 0,1") || strstr(resp, "+CEREG: 0,5")) {
                registered = true;
                break;
            }
        }
        ESP_LOGW(TAG, "modem_ensure_network: waiting for LTE registration...");
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    if (!registered) {
        ESP_LOGE(TAG, "modem_ensure_network: LTE not registered after 30s");
        xSemaphoreGive(s_at_mutex);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "modem_ensure_network: LTE registered");

    // PDP コンテキスト再有効化（失敗しても続行: すでに有効な場合 ERROR が返る）
    at_cmd_send("AT+CGACT=1,1", "OK", 10000);

    // アプリネットワーク設定・再有効化
    at_cmd_send("AT+CNCFG=0,1,\"soracom.io\",\"sora\",\"sora\",1", "OK", 5000);
    esp_err_t ret = at_cmd_send("AT+CNACT=0,1", "OK", 15000);
    vTaskDelay(pdMS_TO_TICKS(1000));   // ネットワーク確立待ち

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "modem_ensure_network: app network re-activated");
    } else {
        ESP_LOGE(TAG, "modem_ensure_network: CNACT failed");
    }

    xSemaphoreGive(s_at_mutex);
    return ret;
}

// ---------- 制御チャンネル UDP API（conn_id=0、ポート6000） ----------

esp_err_t modem_ctrl_open(const char *host, uint16_t port)
{
    xSemaphoreTake(s_at_mutex, portMAX_DELAY);

    // 既存コネクションを念のためクローズ（エラー無視）
    char close_cmd[32];
    snprintf(close_cmd, sizeof(close_cmd), "AT+CACLOSE=%d", CTRL_CONN_ID);
    at_cmd_send(close_cmd, "OK", 3000);
    vTaskDelay(pdMS_TO_TICKS(300));

    // AT+CAOPEN=<connectID>,<pdpidx>,"UDP","<host>",<port>
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+CAOPEN=%d,0,\"UDP\",\"%s\",%u",
             CTRL_CONN_ID, host, port);

    esp_err_t ret = ESP_FAIL;
    char resp[128];
    if (at_cmd_query(cmd, resp, sizeof(resp), 15000) == ESP_OK) {
        char expect[16];
        snprintf(expect, sizeof(expect), "+CAOPEN: %d,0", CTRL_CONN_ID);
        if (strstr(resp, expect)) {
            ESP_LOGI(TAG, "Ctrl UDP open OK: %s:%u", host, port);
            ret = ESP_OK;
        } else {
            ESP_LOGE(TAG, "Ctrl UDP open failed: %s", resp);
        }
    } else {
        ESP_LOGE(TAG, "Ctrl UDP open timeout");
    }

    xSemaphoreGive(s_at_mutex);
    return ret;
}

esp_err_t modem_ctrl_send(const uint8_t *data, size_t len)
{
    xSemaphoreTake(s_at_mutex, portMAX_DELAY);

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CASEND=%d,%zu", CTRL_CONN_ID, len);

    char resp[64];
    esp_err_t ret = at_cmd_query(cmd, resp, sizeof(resp), 5000);
    if (ret != ESP_OK || !strstr(resp, "> ")) {
        ESP_LOGE(TAG, "Ctrl send prompt not received: %s", resp);
        xSemaphoreGive(s_at_mutex);
        return ESP_FAIL;
    }

    at_cmd_write_raw(data, len);

    memset(resp, 0, sizeof(resp));
    size_t pos = 0;
    ret = ESP_FAIL;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(5000);
    while (xTaskGetTickCount() < deadline && pos < sizeof(resp) - 1) {
        int n = uart_read_bytes(CONFIG_MODEM_UART_NUM,
                                (uint8_t *)(resp + pos),
                                sizeof(resp) - pos - 1,
                                pdMS_TO_TICKS(50));
        if (n > 0) {
            pos += n;
            resp[pos] = '\0';
            if (strstr(resp, "OK\r\n") || strstr(resp, "OK\n")) { ret = ESP_OK; break; }
            if (strstr(resp, "ERROR")) break;
        }
    }

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Ctrl send failed: %s", resp);
    } else {
        ESP_LOGD(TAG, "Ctrl sent %zu bytes", len);
    }

    xSemaphoreGive(s_at_mutex);
    return ret;
}

esp_err_t modem_ctrl_recv(uint8_t *buf, size_t max_len, size_t *out_len, uint32_t timeout_ms)
{
    *out_len = 0;

    xSemaphoreTake(s_at_mutex, portMAX_DELAY);

    uart_flush_input(CONFIG_MODEM_UART_NUM);
    char tx[64];
    int tx_len = snprintf(tx, sizeof(tx), "AT+CARECV=%d,%zu\r\n", CTRL_CONN_ID, max_len);
    uart_write_bytes(CONFIG_MODEM_UART_NUM, tx, tx_len);

    // SIM7080G の AT+CARECV レスポンス形式:
    //   "+CARECV: N,<Nバイトのデータ>\r\n\r\nOK\r\n"
    char hdr[512];
    esp_err_t ret = at_cmd_wait_line("+CARECV: ", hdr, sizeof(hdr), timeout_ms);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_at_mutex);
        return ESP_ERR_TIMEOUT;
    }

    char *p = strstr(hdr, "+CARECV: ");
    int n = (p) ? atoi(p + 9) : 0;

    if (n <= 0) {
        char dummy[16];
        at_cmd_wait_line("OK", dummy, sizeof(dummy), 1000);
        xSemaphoreGive(s_at_mutex);
        return ESP_OK;   // データなし（out_len=0）
    }

    size_t to_read = ((size_t)n < max_len) ? (size_t)n : max_len;

    char *comma = (p) ? strchr(p + 9, ',') : NULL;
    if (comma != NULL) {
        memcpy(buf, comma + 1, to_read);
        *out_len = to_read;
        ret = ESP_OK;
        ESP_LOGD(TAG, "Ctrl recv %zu bytes (inline)", *out_len);
    } else {
        size_t data_start = 0;
        uint8_t first;
        if (uart_read_bytes(CONFIG_MODEM_UART_NUM, &first, 1, pdMS_TO_TICKS(500)) == 1) {
            if (first == '\r') {
                uint8_t lf;
                uart_read_bytes(CONFIG_MODEM_UART_NUM, &lf, 1, pdMS_TO_TICKS(50));
            } else {
                buf[0]     = first;
                data_start = 1;
            }
        }
        ret = at_cmd_read_raw(buf + data_start, to_read - data_start, 2000);
        if (ret == ESP_OK) {
            *out_len = to_read;
            ESP_LOGD(TAG, "Ctrl recv %zu bytes", *out_len);
        } else {
            ESP_LOGE(TAG, "Ctrl recv raw read failed");
        }
    }

    char dummy[16];
    at_cmd_wait_line("OK", dummy, sizeof(dummy), 1000);

    xSemaphoreGive(s_at_mutex);
    return ret;
}

esp_err_t modem_ctrl_close(void)
{
    xSemaphoreTake(s_at_mutex, portMAX_DELAY);
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CACLOSE=%d", CTRL_CONN_ID);
    esp_err_t ret = at_cmd_send(cmd, "OK", 5000);
    xSemaphoreGive(s_at_mutex);
    ESP_LOGI(TAG, "Ctrl UDP closed");
    return ret;
}

// ---------- UDP ソケット API ----------

esp_err_t modem_udp_open(const char *host, uint16_t port)
{
    xSemaphoreTake(s_at_mutex, portMAX_DELAY);

    // 既存コネクションを念のためクローズ
    char close_cmd[32];
    snprintf(close_cmd, sizeof(close_cmd), "AT+CACLOSE=%d", UDP_CONN_ID);
    at_cmd_send(close_cmd, "OK", 3000);
    vTaskDelay(pdMS_TO_TICKS(200));

    // AT+CAOPEN=<connectID>,<pdpidx>,"UDP","<host>",<port>
    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+CAOPEN=%d,0,\"UDP\",\"%s\",%u",
             UDP_CONN_ID, host, port);

    esp_err_t ret = ESP_FAIL;
    char resp[128];
    if (at_cmd_query(cmd, resp, sizeof(resp), 15000) == ESP_OK) {
        char expect[16];
        snprintf(expect, sizeof(expect), "+CAOPEN: %d,0", UDP_CONN_ID);
        if (strstr(resp, expect)) {
            ESP_LOGI(TAG, "UDP open OK: %s:%u", host, port);
            ret = ESP_OK;
        } else {
            ESP_LOGE(TAG, "UDP open failed: %s", resp);
        }
    } else {
        ESP_LOGE(TAG, "UDP open timeout");
    }

    xSemaphoreGive(s_at_mutex);
    return ret;
}

esp_err_t modem_udp_send(const uint8_t *data, size_t len)
{
    xSemaphoreTake(s_at_mutex, portMAX_DELAY);

    // 前フレームの OK\r\n や URC (+CADATAIND 等) が UART バッファに残っている
    // 可能性があるのでフラッシュしてから AT+CASEND を発行する。
    // これをしないと pbuf が OK\r\n で埋まり '>' を見つけられなくなる。
    uart_flush_input(CONFIG_MODEM_UART_NUM);

    char cmd[32];
    int cmdlen = snprintf(cmd, sizeof(cmd), "AT+CASEND=%d,%zu\r\n", UDP_CONN_ID, len);
    uart_write_bytes(CONFIG_MODEM_UART_NUM, cmd, cmdlen);

    // '>' プロンプト待ち
    // pbuf を 128 バイトに拡張: エコー(ATE0 失敗時 ~18B) + URC (~15B) + "> "(2B) を吸収
    char pbuf[128];
    int total = 0;
    bool got_prompt = false;
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(300);
    while (xTaskGetTickCount() < deadline && total < (int)sizeof(pbuf) - 1) {
        int n = uart_read_bytes(CONFIG_MODEM_UART_NUM,
                                (uint8_t *)(pbuf + total),
                                sizeof(pbuf) - total - 1,
                                pdMS_TO_TICKS(10));
        if (n > 0) {
            total += n;
            pbuf[total] = '\0';
            if (strchr(pbuf, '>')) { got_prompt = true; break; }
        }
    }

    if (!got_prompt) {
        ESP_LOGW(TAG, "UDP send: no prompt (len=%zu buf=%d [%.*s])",
                 len, total, total > 32 ? 32 : total, pbuf);
        xSemaphoreGive(s_at_mutex);
        return ESP_FAIL;
    }

    uart_write_bytes(CONFIG_MODEM_UART_NUM, (const char *)data, len);

    // OK 応答を消費する。これをしないと OK\r\n が次フレームの uart_flush_input 前に
    // バッファに残り、タイミング次第でフラッシュをすり抜けて次の AT+CASEND の
    // プロンプト読み取りを妨害する（交互失敗パターンの根本原因）。
    char okbuf[32];
    int oktotal = 0;
    TickType_t okdeadline = xTaskGetTickCount() + pdMS_TO_TICKS(80);
    while (xTaskGetTickCount() < okdeadline && oktotal < (int)sizeof(okbuf) - 1) {
        int n = uart_read_bytes(CONFIG_MODEM_UART_NUM,
                                (uint8_t *)(okbuf + oktotal),
                                sizeof(okbuf) - oktotal - 1,
                                pdMS_TO_TICKS(10));
        if (n > 0) {
            oktotal += n;
            okbuf[oktotal] = '\0';
            if (strstr(okbuf, "OK") || strstr(okbuf, "ERROR")) break;
        }
    }

    xSemaphoreGive(s_at_mutex);
    return ESP_OK;
}

esp_err_t modem_udp_recv(uint8_t *buf, size_t max_len, size_t *out_len, uint32_t timeout_ms)
{
    *out_len = 0;

    xSemaphoreTake(s_at_mutex, portMAX_DELAY);

    uart_flush_input(CONFIG_MODEM_UART_NUM);
    char tx[64];
    int tx_len = snprintf(tx, sizeof(tx), "AT+CARECV=%d,%zu\r\n", UDP_CONN_ID, max_len);
    uart_write_bytes(CONFIG_MODEM_UART_NUM, tx, tx_len);

    // +CARECV: N,<データ> または +CARECV: 0 を受信
    char hdr[512];
    esp_err_t ret = at_cmd_wait_line("+CARECV: ", hdr, sizeof(hdr), timeout_ms);
    if (ret != ESP_OK) {
        xSemaphoreGive(s_at_mutex);
        return ESP_ERR_TIMEOUT;
    }

    char *p = strstr(hdr, "+CARECV: ");
    int n = (p) ? atoi(p + 9) : 0;

    if (n <= 0) {
        char dummy[16];
        at_cmd_wait_line("OK", dummy, sizeof(dummy), 1000);
        xSemaphoreGive(s_at_mutex);
        return ESP_OK;   // データなし（out_len=0）
    }

    size_t to_read = ((size_t)n < max_len) ? (size_t)n : max_len;

    char *comma = (p) ? strchr(p + 9, ',') : NULL;
    if (comma != NULL) {
        // インライン形式: "+CARECV: N,<data>" — カンマ直後がデータ先頭
        memcpy(buf, comma + 1, to_read);
        *out_len = to_read;
        ret = ESP_OK;
        ESP_LOGD(TAG, "UDP recv %zu bytes (inline)", *out_len);
    } else {
        // 改行区切り形式: "+CARECV: N\r\n<data>"
        size_t data_start = 0;
        uint8_t first;
        if (uart_read_bytes(CONFIG_MODEM_UART_NUM, &first, 1, pdMS_TO_TICKS(500)) == 1) {
            if (first == '\r') {
                uint8_t lf;
                uart_read_bytes(CONFIG_MODEM_UART_NUM, &lf, 1, pdMS_TO_TICKS(50));
            } else {
                buf[0]     = first;
                data_start = 1;
            }
        }
        ret = at_cmd_read_raw(buf + data_start, to_read - data_start, 2000);
        if (ret == ESP_OK) {
            *out_len = to_read;
            ESP_LOGD(TAG, "UDP recv %zu bytes", *out_len);
        } else {
            ESP_LOGE(TAG, "UDP recv raw read failed");
        }
    }

    char dummy[16];
    at_cmd_wait_line("OK", dummy, sizeof(dummy), 1000);

    xSemaphoreGive(s_at_mutex);
    return ret;
}

esp_err_t modem_udp_close(void)
{
    xSemaphoreTake(s_at_mutex, portMAX_DELAY);
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CACLOSE=%d", UDP_CONN_ID);
    esp_err_t ret = at_cmd_send(cmd, "OK", 5000);
    xSemaphoreGive(s_at_mutex);
    ESP_LOGI(TAG, "UDP closed");
    return ret;
}
