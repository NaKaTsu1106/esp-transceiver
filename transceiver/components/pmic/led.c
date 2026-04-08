#include "led.h"
#include "axp2101.h"
#include "esp_log.h"

static const char *TAG = "led";

#define AXP2101_REG_CHGLED  0x69
#define AXP2101_CHGLED_MASK 0x37    // 変更可能ビット（bit0-2, bit4-5）
                                    // bit3, bit6-7 は他機能用のため変更禁止

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

esp_err_t led_set(uint8_t mode)
{
    // Read-Modify-Write（仕様書 3.2節）
    uint8_t val;
    esp_err_t ret = axp2101_read(AXP2101_REG_CHGLED, &val);
    if (ret != ESP_OK) return ret;

    val &= ~AXP2101_CHGLED_MASK;        // マスク対象ビットをクリア
    val |= (mode & AXP2101_CHGLED_MASK); // 手動制御モード + LED状態を書き込み
    return axp2101_write(AXP2101_REG_CHGLED, val);
}
