#pragma once

// =============================================================================
// サーバー設定
// =============================================================================
// CONFIG_SERVER_IP は menuconfig で設定（Kconfig.projbuild）
// CONFIG_GROUP_ID  は menuconfig で設定（Kconfig.projbuild）
#define CONFIG_CTRL_PORT        6000   // 制御チャンネル（UDP）
#define CONFIG_UDP_PORT         6001   // 音声チャンネル（UDP）

// =============================================================================
// モデム設定
// =============================================================================
#define CONFIG_MODEM_UART_NUM   UART_NUM_2
#define CONFIG_MODEM_TXD_PIN    5
#define CONFIG_MODEM_RXD_PIN    4
#define CONFIG_MODEM_PWR_PIN    41
#define CONFIG_MODEM_DTR_PIN    42
#define CONFIG_MODEM_RI_PIN     3
#define CONFIG_MODEM_BAUD_RATE  921600
#define CONFIG_MODEM_BAUD_FALLBACK 115200

// =============================================================================
// AXP2101 PMIC 設定
// =============================================================================
#define CONFIG_PMIC_I2C_NUM     I2C_NUM_0
#define CONFIG_PMIC_SDA_PIN     15
#define CONFIG_PMIC_SCL_PIN     7
#define CONFIG_PMIC_IRQ_PIN     6
#define CONFIG_PMIC_I2C_ADDR    0x34    // 7ビットアドレス（仕様書の0x68は8ビット表記）
#define CONFIG_PMIC_I2C_FREQ    400000      // 400kHz

// =============================================================================
// I2S ピン設定（TX/RX を別ペリフェラルに分離）
// =============================================================================
// TX: PCM5102A スピーカー出力（I2S_NUM_0）
#define CONFIG_I2S_TX_BCK_PIN       11  // Bit Clock
#define CONFIG_I2S_TX_WS_PIN         9  // Word Select (L/R Clock)
#define CONFIG_I2S_TX_DOUT_PIN      10  // データ出力

// RX: INMP441 MEMS マイク入力（I2S_NUM_1）
// INMP441 は MCLK 不要・モノラル I2S 出力（L/R ピン GND = LEFT チャンネル）
#define CONFIG_I2S_RX_BCK_PIN       18  // Bit Clock
#define CONFIG_I2S_RX_LR_PIN        17  // LR
#define CONFIG_I2S_RX_WS_PIN        16  // Word Select (L/R Clock)
#define CONFIG_I2S_RX_DIN_PIN        8  // データ入力

// =============================================================================
// 音声設定
// =============================================================================
#define CONFIG_AUDIO_SAMPLE_RATE    16000   // 16kHz
#define CONFIG_AUDIO_CHANNELS       1       // モノラル
#define CONFIG_OPUS_BITRATE         16000   // 16kbps
#define CONFIG_OPUS_FRAME_MS        20      // 20ms
#define CONFIG_OPUS_FRAME_SAMPLES   (CONFIG_AUDIO_SAMPLE_RATE * CONFIG_OPUS_FRAME_MS / 1000) // 320

// =============================================================================
// 入力設定
// =============================================================================
// PTT: GPIO14 デジタル入力（内部プルアップ）
// スイッチ押下で GND に落ちる（LOW = 押下）。CONFIG_PTT_HOLD_MS 継続で確定。
#define CONFIG_PTT_GPIO         14    // GPIO ピン番号
#define CONFIG_PTT_HOLD_MS      100   // チャタリング除去・押下確定時間 (ms)

// =============================================================================
// ネットワーク設定
// =============================================================================
#define CONFIG_HEARTBEAT_INTERVAL_MS    25000   // 25秒
#define CONFIG_UDP_KEEPALIVE_INTERVAL_MS 25000  // 25秒
#define CONFIG_PTT_ACK_TIMEOUT_MS       3000    // 3秒
#define CONFIG_PTT_MAX_HOLD_MS          60000   // 60秒（最大送話時間）
#define CONFIG_CTRL_RETRY_MAX_MS        30000   // 最大バックオフ30秒

// =============================================================================
// FreeRTOS キュー設定
// =============================================================================
#define CONFIG_PCM_ENCODE_QUEUE_LEN     4
#define CONFIG_ENCODED_TX_QUEUE_LEN     8
#define CONFIG_RECEIVED_RX_QUEUE_LEN    8
#define CONFIG_PCM_PLAYBACK_QUEUE_LEN   4
#define CONFIG_CTRL_TX_QUEUE_LEN        8
#define CONFIG_CTRL_RX_QUEUE_LEN        8

// =============================================================================
// ジッタバッファ設定
// =============================================================================
#define CONFIG_JITTER_BUF_START_FRAMES  3   // 再生開始に必要な最低フレーム数
