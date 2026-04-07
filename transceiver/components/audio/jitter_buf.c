#include "jitter_buf.h"
#include "config.h"

esp_err_t jitter_buf_init(void)
{
    // TODO: Step 7で実装
    return ESP_OK;
}

esp_err_t jitter_buf_push(const uint8_t *opus_data, size_t len, uint16_t seq)
{
    // TODO: Step 7で実装
    return ESP_OK;
}

esp_err_t jitter_buf_pop(uint8_t *opus_data, size_t *len)
{
    // TODO: Step 7で実装
    return ESP_ERR_NOT_FOUND;
}

bool jitter_buf_ready(void)
{
    // TODO: Step 7で実装
    return false;
}

void jitter_buf_flush(void)
{
    // TODO: Step 7で実装
}
