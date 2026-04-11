#include "jitter_buf.h"
#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>

#define JITTER_FRAME_MAX  256   // 最大 Opus フレームサイズ（バイト）

typedef struct {
    uint8_t data[JITTER_FRAME_MAX];
    size_t  len;
} jitter_entry_t;

static jitter_entry_t     s_buf[CONFIG_RECEIVED_RX_QUEUE_LEN];
static int                s_head    = 0;   // pop 側
static int                s_tail    = 0;   // push 側
static int                s_count   = 0;
static bool               s_started = false;  // 初回バッファリング完了フラグ
static SemaphoreHandle_t  s_mutex   = NULL;

esp_err_t jitter_buf_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
        if (s_mutex == NULL) return ESP_FAIL;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_head = s_tail = s_count = 0;
    s_started = false;
    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t jitter_buf_push(const uint8_t *opus_data, size_t len, uint16_t seq)
{
    (void)seq;  // 現実装では seq は FIFO 順序で暗黙管理
    if (len > JITTER_FRAME_MAX) len = JITTER_FRAME_MAX;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_count >= CONFIG_RECEIVED_RX_QUEUE_LEN) {
        // バッファ満杯: 最古フレームを捨てて上書き（バースト遅延吸収）
        s_head = (s_head + 1) % CONFIG_RECEIVED_RX_QUEUE_LEN;
        s_count--;
    }

    memcpy(s_buf[s_tail].data, opus_data, len);
    s_buf[s_tail].len = len;
    s_tail = (s_tail + 1) % CONFIG_RECEIVED_RX_QUEUE_LEN;
    s_count++;

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

esp_err_t jitter_buf_pop(uint8_t *opus_data, size_t *len)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (s_count == 0) {
        xSemaphoreGive(s_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    *len = s_buf[s_head].len;
    memcpy(opus_data, s_buf[s_head].data, *len);
    s_head = (s_head + 1) % CONFIG_RECEIVED_RX_QUEUE_LEN;
    s_count--;

    xSemaphoreGive(s_mutex);
    return ESP_OK;
}

bool jitter_buf_ready(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    // CONFIG_JITTER_BUF_START_FRAMES 溜まったら再生開始。
    // 一度 started になったら flush() まで true を保持し、
    // パケットロス中も PLC が継続できるようにする。
    if (!s_started && s_count >= CONFIG_JITTER_BUF_START_FRAMES) {
        s_started = true;
    }
    bool ready = s_started;
    xSemaphoreGive(s_mutex);
    return ready;
}

void jitter_buf_flush(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_head    = 0;
    s_tail    = 0;
    s_count   = 0;
    s_started = false;
    xSemaphoreGive(s_mutex);
}
