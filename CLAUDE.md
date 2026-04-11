# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

LilyGO T-SIM7080G-S3（ESP32-S3）を使ったIPトランシーバー。LTE-M経由でVPS中継サーバーを介し、複数端末間でPTT（Push-to-Talk）音声通信を行う。

- **デバイス側**: C（ESP-IDF v6.0）→ `transceiver/`
- **サーバー側**: Python → `server/`
- **仕様書**: `doc/specification.md`
- **実装進捗**: `doc/progress.md`

## Build Commands

### サーバー（Python）

```bash
cd server
pip install -r requirements.txt
python main.py
```

## Architecture

### デバイス側コンポーネント構成

`transceiver/main/config.h` が全設定定数（GPIO・ポート・タイムアウト等）の唯一の真実の源。実機に合わせて `CONFIG_SERVER_IP` を設定すること。

**コア設計原則**:
- **Core 0（PRO_CPU）**: 通信系タスク（modem / tcp / udp / heartbeat）
- **Core 1（APP_CPU）**: 音声系タスク（i2s / opus / ptt / led / state_machine）
- タスク間通信はFreeRTOSキューとイベントグループ（`g_system_events`）のみ。共有メモリへの直接アクセス禁止。

**イベントグループ** (`state_machine.h`):
```
EVT_MODEM_READY / EVT_CONNECTED / EVT_PTT_PRESSED /
EVT_FLOOR_GRANTED / EVT_FLOOR_BUSY / EVT_FLOOR_FREE / EVT_DISCONNECTED
```

**音声パイプライン**（送信）:
```
i2s_capture_task → pcm_encode_queue → opus_encode_task → encoded_tx_queue → udp_tx_task
```

**音声パイプライン**（受信）:
```
udp_rx_task → received_rx_queue（ジッタバッファ） → opus_decode_task → pcm_playback_queue → i2s_playback_task
```

**モデム通信の原則**: ATコマンドは `modem_task` が排他管理。他タスクがUARTに直接アクセスしてはならない。

### サーバー側モジュール構成

```
main.py          エントリポイント。各サーバー起動・タイムアウト監視
config.py        設定読み込み（環境変数・デフォルト値）
protocol.py      制御メッセージの定義・シリアライズ（デバイス側 protocol.h と対応）
device.py        端末管理（セッションID・UDPアドレスの動的更新）
floor.py         グループ単位の送話権（Floor Control）管理
ctrl_server.py   UDP制御チャンネルサーバー（port 6000）
audio_server.py  UDP音声中継サーバー（port 6001）
web_server.py    WebSocketサーバー（port 8080）
monitor.py       WebSocketブロードキャスト管理・ログ管理
```

### プロトコル

**TCP制御チャンネル（ポート6000）**: `[1B: ペイロード長][1B: タイプ][ペイロード]`

**UDP音声チャンネル（ポート6001）**: `[1B: セッションID][1B: フラグ][2B: シーケンス][2B: タイムスタンプ][ペイロード]`
- フラグ: bit7=パケット種別（0=AUDIO/1=KEEPALIVE）、bit3-0=グループID

## Known Issues & Constraints

- **GCC 15.2.0のコンパイラICE**: `esp_lcd_panel_rgb.c` でクラッシュするため、`transceiver/CMakeLists.txt` で `set(EXCLUDE_COMPONENTS esp_lcd)` を設定済み。
- **AXP2101 BLDO1は絶対に無効化しない**: SIM7080G↔ESP32間のUARTレベルシフタ電源。無効化するとモデムと通信不能になる。
- **`config.h` のインクルード**: `config.h` は `transceiver/main/` にある。各コンポーネントの `CMakeLists.txt` の `INCLUDE_DIRS` に `"${CMAKE_SOURCE_DIR}/main"` を追加することで参照可能。
- **ESP-IDF v6 ドライバコンポーネント名**: `driver` は使わず、ペリフェラルごとに個別コンポーネントを `REQUIRES` に指定する。
  - I2C: `esp_driver_i2c`（ヘッダ: `driver/i2c_master.h`）
  - UART: `esp_driver_uart`（ヘッダ: `driver/uart.h`）
  - GPIO: `esp_driver_gpio`（ヘッダ: `driver/gpio.h`）
  - 新I2C APIは7ビットアドレスを使用する（仕様書の `0x68` は8ビット表記、7ビットは `0x34`）。
- **`esp-libopus`**: `codec` コンポーネントはStep 6実装時まで依存不要。使用時は `idf_component.yml` に `espressif/esp-libopus` を追加し、`CMakeLists.txt` の `REQUIRES` に追記する。

## Implementation Steps

`doc/progress.md` を参照。各ステップはスタブ（`// TODO: Step Nで実装`）として骨格が存在しており、順番に実装する。
