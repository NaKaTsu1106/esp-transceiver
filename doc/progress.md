# 実装進捗

最終更新: 2026-04-09

## ステップ一覧

| ステップ | 内容 | 状態 |
|---------|------|------|
| Step 1 | プロジェクト構成の整備 | ✅ 完了 |
| Step 2 | PMIC・LED（AXP2101） | ✅ 完了 |
| Step 3 | モデム起動・LTE-M接続 | ✅ 完了 |
| Step 4 | TCPサーバー（Go）+ TCP接続（デバイス） | ✅ 完了 |
| Step 5 | PTT制御 | ⬜ 未着手 |
| Step 6 | 音声送信パイプライン | ✅ 完了 |
| Step 7 | 音声受信パイプライン | ⬜ 未着手 |
| Step 8 | エラー処理・再接続 | ⬜ 未着手 |
| Step 9 | デバッグモニター | ⬜ 未着手 |

---

## Step 1: プロジェクト構成の整備 ✅

### 完了タスク

- [x] デバイス: `config.h` 作成
- [x] デバイス: コンポーネントディレクトリ構造作成
- [x] デバイス: 各コンポーネントのスタブ作成
- [x] デバイス: `main.c` 起動シーケンス骨格作成
- [x] デバイス: `CMakeLists.txt` 更新（全コンポーネント登録）
- [x] サーバー: Goプロジェクト構造作成
- [x] サーバー: `go.mod` 初期化
- [x] サーバー: 各パッケージのスタブ作成

### 作成ファイル一覧

#### デバイス側（transceiver/）
```
main/
  main.c          起動シーケンス骨格
  config.h        全設定定数
  CMakeLists.txt  コンポーネント登録済み

components/
  pmic/   axp2101.c/h, led.c/h, CMakeLists.txt
  modem/  at_cmd.c/h, modem.c/h, CMakeLists.txt
  network/ tcp_client.c/h, udp_client.c/h, CMakeLists.txt
  protocol/ protocol.c/h, floor_ctrl.c/h, CMakeLists.txt
  audio/  i2s_capture.c/h, i2s_playback.c/h, jitter_buf.c/h, CMakeLists.txt
  codec/  opus_codec.c/h, CMakeLists.txt
  state/  state_machine.c/h, CMakeLists.txt
```

#### サーバー側（server/）
```
main.go
go.mod
config/config.go
protocol/protocol.go
device/device.go
floor/floor.go
server/tcp_server.go, udp_server.go, ws_server.go
audio/relay.go, testtone.go
monitor/monitor.go, logger.go
web/static/  （Step 9で作成）
```

### 備考
- 全コンポーネントはスタブ（空実装）。各ステップで順次実装する
- `config.h` の `CONFIG_SERVER_IP` は実際のVPS IPに変更が必要

---

## Step 6: 音声送信パイプライン ✅

### 完了タスク

- [x] `config.h`: I2S GPIO ピン定義追加（BCK=12, WS=13, DIN=11, DOUT=10）
- [x] `state_machine.h`: `pcm_frame_t`, `encoded_frame_t`, `g_pcm_encode_queue`, `g_encoded_tx_queue` 追加
- [x] `state_machine.c`: 音声キュー作成（`state_machine_init()`）
- [x] `i2s_capture.c`: INMP441実装（8kHz, 32bit slot, 16bit変換）
- [x] `audio/CMakeLists.txt`: `driver` → `esp_driver_i2s`, `state` 追加
- [x] `opus_codec.c`: エンコーダ実装（8kbps, complexity=3, `opus_encode_task`）
- [x] `codec/CMakeLists.txt`: `espressif__esp-libopus`, `state` 追加
- [x] `codec/idf_component.yml`: esp-libopus マネージドコンポーネント依存追加
- [x] `modem.h/c`: `modem_udp_open()`, `modem_udp_send()`, `modem_udp_close()` 実装（コネクションID=1）
- [x] `udp_client.c`: `udp_tx_task` 実装（EVT_CONNECTED待機、キープアライブ25s）

### 音声パイプライン（送信）

```
i2s_capture_task → [g_pcm_encode_queue] → opus_encode_task → [g_encoded_tx_queue] → udp_tx_task
```

### 備考
- I2S GPIO ピン番号はプレースホルダー。実機に合わせて `config.h` を変更すること
- `udp_tx_task` は `EVT_FLOOR_GRANTED` が立っているフレームのみ送信
- `i2s_capture_task` は常時録音するが、`EVT_FLOOR_GRANTED` 時のみキューへプッシュ

---

## Step 2: PMIC・LED（AXP2101） ⬜

### タスク
- [ ] I2C初期化（GPIO15=SDA, GPIO7=SCL, 400kHz）
- [ ] AXP2101疎通確認（アドレス0x68）
- [ ] BLDO1有効化（レベルシフタ電源）
- [ ] DCDC3有効化（SIM7080G電源）
- [ ] LED制御実装（レジスタ0x69, Read-Modify-Write）
- [ ] `led_task` 実装（イベントグループ監視）
- [ ] 動作確認: LEDが1Hz点滅すること

### 確認方法
- `idf.py monitor` でログ確認
- LEDが1Hz点滅すること
