#pragma once
#include "esp_err.h"
#include "driver/i2s_std.h"
#include <stdint.h>
#include <stddef.h>

// i2s_capture_init() が TX (PCM5102) と RX (PCM1808) を同一 I2S で初期化する。
// i2s_playback は TX チャンネルハンドルをこの関数で取得して使用する。
esp_err_t          i2s_capture_init(void);
i2s_chan_handle_t  i2s_get_tx_chan(void);

esp_err_t i2s_capture_read(int16_t *buf, size_t samples, size_t *bytes_read);
void      i2s_capture_task(void *arg);
