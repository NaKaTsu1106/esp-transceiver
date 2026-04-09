#include "led.h"
#include "axp2101.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "led";

#define AXP2101_REG_CHGLED      0x69
#define AXP2101_CHGLED_PRESERVE 0xC8  // 保持するビット: bit3, bit6-7（予約）
#define AXP2101_CHGLED_MAN      0x05  // bit2=MAN, bit0=AUTO（手動制御に必須）

esp_err_t led_init(void)
{
    // axp2101_init()完了後に呼ぶ前提。消灯で初期化
    esp_err_t ret = led_set(CHGLED_OFF);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "led_init failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "led_init OK");
    return ESP_OK;
}

void led_test(void)
{
    ESP_LOGI(TAG, "LED test start");

    // 点灯 → 4Hz点滅 → 1Hz点滅 → 消灯 の順で確認（2Hzモードは存在しない）
    static const struct { uint8_t mode; const char *label; uint32_t ms; } seq[] = {
        { CHGLED_ON,  "ON",  2000 },
        { CHGLED_4HZ, "4Hz", 2000 },
        { CHGLED_1HZ, "1Hz", 2000 },
        { CHGLED_OFF, "OFF", 2000 },
    };

    for (int i = 0; i < (int)(sizeof(seq) / sizeof(seq[0])); i++) {
        ESP_LOGI(TAG, "LED test: %s", seq[i].label);
        led_set(seq[i].mode);
        vTaskDelay(pdMS_TO_TICKS(seq[i].ms));
    }

    ESP_LOGI(TAG, "LED test done");
}

esp_err_t led_set(uint8_t mode)
{
    // Read-Modify-Write（XPowersLib setChargingLedMode() 準拠）
    // bit5-4: LED状態, bit2=MAN, bit0=AUTO を設定。bit3,6,7 は保持。
    uint8_t val;
    esp_err_t ret = axp2101_read(AXP2101_REG_CHGLED, &val);
    if (ret != ESP_OK) return ret;

    val &= AXP2101_CHGLED_PRESERVE;   // 予約ビット(bit3,6,7)のみ保持
    val |= AXP2101_CHGLED_MAN;        // bit2=MAN, bit0=AUTO セット
    val |= ((mode & 0x03) << 4);      // LED状態を bit5-4 に配置
    return axp2101_write(AXP2101_REG_CHGLED, val);
}
