#pragma once
#include "esp_err.h"

// AXP2101 レジスタ0x69 手動制御モード定数
// bit5-4: LED状態, bit2: MAN=1(手動制御), bit0: AUTO=1(固定)
// 例: OFF=0x05(00000101), 1Hz=0x15(00010101), 4Hz=0x25(00100101), ON=0x35(00110101)
// XPowersLib setChargingLedMode() と同一仕様
// ※ 2Hzモードは AXP2101 に存在しない
#define CHGLED_OFF   0   // LED状態フィールド値（bit5-4 にシフトして使用）
#define CHGLED_1HZ   1
#define CHGLED_4HZ   2
#define CHGLED_ON    3

esp_err_t led_init(void);
esp_err_t led_set(uint8_t mode);
void      led_test(void);  // 初期化時LED動作確認シーケンス
