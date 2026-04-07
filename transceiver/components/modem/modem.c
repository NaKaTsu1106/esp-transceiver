#include "modem.h"
#include "at_cmd.h"
#include "led.h"
#include "config.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "modem";

#define MODEM_RETRY_MAX  3

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
