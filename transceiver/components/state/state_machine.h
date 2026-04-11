#pragma once
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "protocol.h"
#include "config.h"

// イベントビット定義
#define EVT_MODEM_READY     (1 << 0)
#define EVT_CONNECTED       (1 << 1)
#define EVT_PTT_PRESSED     (1 << 2)
#define EVT_FLOOR_GRANTED   (1 << 3)
#define EVT_FLOOR_BUSY      (1 << 4)
#define EVT_FLOOR_FREE      (1 << 5)
#define EVT_DISCONNECTED    (1 << 6)

extern EventGroupHandle_t g_system_events;

// TCP制御メッセージキュー
// g_ctrl_tx_queue: tcp_task が送信すべきメッセージをプッシュ（heartbeat_task など → tcp_task）
// g_ctrl_rx_queue: tcp_task が受信したメッセージをプッシュ（tcp_task → state_machine_task）
extern QueueHandle_t g_ctrl_tx_queue;
extern QueueHandle_t g_ctrl_rx_queue;

// 音声パイプラインキュー（送信）
// g_pcm_encode_queue:  i2s_capture_task → opus_encode_task (pcm_frame_t)
// g_encoded_tx_queue:  opus_encode_task → udp_tx_task       (encoded_frame_t)
extern QueueHandle_t g_pcm_encode_queue;
extern QueueHandle_t g_encoded_tx_queue;

// 音声パイプラインキュー（受信）
// g_pcm_playback_queue: opus_decode_task → i2s_playback_task (pcm_frame_t)
extern QueueHandle_t g_pcm_playback_queue;

// ビープリクエストキュー: state_machine → i2s_playback_task
// i2s_playback_task 内で正弦波を生成・ミックスすることで
// g_pcm_playback_queue の輻輳に依存せず確実に再生する。
typedef struct {
    int  freq_hz;
    int  duration_ms;
    bool deferred;  // true: g_pcm_playback_queue が空になるまで待ってから再生
} beep_request_t;
extern QueueHandle_t g_beep_queue;

// PCMフレーム（20ms @ 16kHz = 320サンプル）
typedef struct {
    int16_t samples[CONFIG_OPUS_FRAME_SAMPLES];
} pcm_frame_t;

// エンコード済みフレーム
typedef struct {
    uint8_t  data[64];
    int      len;
    uint16_t seq;
    uint16_t timestamp_ms;
} encoded_frame_t;

void state_machine_init(void);
void state_set(EventBits_t bits);
void state_clear(EventBits_t bits);
EventBits_t state_get(void);
void state_machine_task(void *arg);
void ptt_task(void *arg);
void led_task(void *arg);
void heartbeat_task(void *arg);
