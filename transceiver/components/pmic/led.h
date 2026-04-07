#pragma once
#include "esp_err.h"

// AXP2101 レジスタ0x69 手動制御モード定数
#define CHGLED_OFF   0x30
#define CHGLED_1HZ   0x31
#define CHGLED_2HZ   0x32
#define CHGLED_4HZ   0x33
#define CHGLED_ON    0x35

esp_err_t led_init(void);
esp_err_t led_set(uint8_t mode);
