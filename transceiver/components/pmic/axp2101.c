#include "axp2101.h"
#include "config.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "axp2101";

// ---------- レジスタ定義 ----------

#define AXP2101_REG_CHIP_ID     0x03    // チップID（期待値: 0x4A）
#define AXP2101_CHIP_ID         0x4A

// 電源チャンネル有効化
#define AXP2101_REG_DCDC_EN     0x80    // DCDCチャンネル有効化
#define AXP2101_REG_LDO_EN      0x90    // LDO/BLDOチャンネル有効化
#define AXP2101_DCDC3_EN_BIT    (1 << 2)
#define AXP2101_BLDO1_EN_BIT    (1 << 4)

// 電圧設定
#define AXP2101_REG_DCDC3_VOL   0x84    // DCDC3電圧設定
#define AXP2101_REG_BLDO1_VOL   0x96    // BLDO1電圧設定

// 電圧計算
// BLDO1: 500mV + n*100mV → 3300mV = (3300-500)/100 = 0x1C
#define AXP2101_BLDO1_3300MV    ((3300 - 500) / 100)
// DCDC3: 1600mV + (n-0x58)*100mV → 3000mV = (3000-1600)/100 + 0x58 = 0x66t-
#define AXP2101_DCDC3_3000MV    ((3000 - 1600) / 100 + 0x58)

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;

esp_err_t axp2101_init(void)
{
    esp_err_t ret;

    // I2Cバス初期化
    i2c_master_bus_config_t bus_cfg = {
        .clk_source            = I2C_CLK_SRC_DEFAULT,
        .i2c_port              = CONFIG_PMIC_I2C_NUM,
        .scl_io_num            = CONFIG_PMIC_SCL_PIN,
        .sda_io_num            = CONFIG_PMIC_SDA_PIN,
        .glitch_ignore_cnt     = 7,
        .flags.enable_internal_pullup = true,
    };
    ret = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = CONFIG_PMIC_I2C_ADDR,
        .scl_speed_hz    = CONFIG_PMIC_I2C_FREQ,
    };
    ret = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C device add failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // 2-1: AXP2101存在確認
    uint8_t chip_id;
    ret = axp2101_read(AXP2101_REG_CHIP_ID, &chip_id);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "AXP2101 not found (addr=0x%02X): %s",
                 CONFIG_PMIC_I2C_ADDR, esp_err_to_name(ret));
        return ret;
    }
    if (chip_id != AXP2101_CHIP_ID) {
        ESP_LOGE(TAG, "chip ID mismatch: got=0x%02X expected=0x%02X",
                 chip_id, AXP2101_CHIP_ID);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "AXP2101 found (chip ID: 0x%02X)", chip_id);

    // 2-2: BLDO1 電圧設定（3300mV）+ 有効化（レベルシフタ電源、無効化禁止）
    ret = axp2101_write(AXP2101_REG_BLDO1_VOL, AXP2101_BLDO1_3300MV);
    if (ret != ESP_OK) return ret;
    uint8_t ldo_en;
    ret = axp2101_read(AXP2101_REG_LDO_EN, &ldo_en);
    if (ret != ESP_OK) return ret;
    ret = axp2101_write(AXP2101_REG_LDO_EN, ldo_en | AXP2101_BLDO1_EN_BIT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BLDO1 enable failed");
        return ret;
    }
    ESP_LOGI(TAG, "BLDO1: 3300mV enabled");

    // 2-3: DCDC3 電圧設定（3000mV）+ コールドスタート + 有効化（SIM7080G電源）
    // 冷起動: すでにONなら一旦OFFにしてから電圧設定・再投入する
    uint8_t dcdc_en;
    ret = axp2101_read(AXP2101_REG_DCDC_EN, &dcdc_en);
    if (ret != ESP_OK) return ret;
    if (dcdc_en & AXP2101_DCDC3_EN_BIT) {
        axp2101_write(AXP2101_REG_DCDC_EN, dcdc_en & ~AXP2101_DCDC3_EN_BIT);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    ret = axp2101_write(AXP2101_REG_DCDC3_VOL, AXP2101_DCDC3_3000MV);
    if (ret != ESP_OK) return ret;
    ret = axp2101_read(AXP2101_REG_DCDC_EN, &dcdc_en);
    if (ret != ESP_OK) return ret;
    ret = axp2101_write(AXP2101_REG_DCDC_EN, dcdc_en | AXP2101_DCDC3_EN_BIT);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DCDC3 enable failed");
        return ret;
    }
    ESP_LOGI(TAG, "DCDC3: 3000mV enabled (SIM7080G power)");

    // 2-4: 100ms待機（モデム電源安定待ち）
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_LOGI(TAG, "axp2101_init OK");
    return ESP_OK;
}

esp_err_t axp2101_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(s_dev, buf, sizeof(buf), -1);
}

esp_err_t axp2101_read(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(s_dev, &reg, 1, val, 1, -1);
}
