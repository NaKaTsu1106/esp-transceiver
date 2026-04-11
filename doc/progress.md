# 実装進捗

最終更新: 2026-04-11（音声品質改善・Step 5完了）

## ステップ一覧

| ステップ | 内容 | 状態 |
|---------|------|------|
| Step 1 | プロジェクト構成の整備 | ✅ 完了 |
| Step 2 | PMIC・LED（AXP2101） | ✅ 完了 |
| Step 3 | モデム起動・LTE-M接続 | ✅ 完了 |
| Step 4 | リレーサーバー（Python）+ UDP制御接続（デバイス） | ✅ 完了 |
| Step 5-1 | PTT動作確認（ptt_task → PTT_START → floor grant） | ✅ 完了 |
| Step 5-2 | 音声送信確認（UDP audio パケットがサーバーに届く） | ✅ 完了 |
| Step 7-1 | `modem_udp_recv` 実装 | ⬜ 未着手 |
| Step 7-2 | `udp_rx_task` 実装（受信・ジッタバッファ投入） | ⬜ 未着手 |
| Step 7-3 | `opus_decode_task` 実装 | ⬜ 未着手 |
| Step 7-4 | `i2s_playback_task` 実装 | ⬜ 未着手 |
| Step 7-5 | エンドツーエンド動作確認 | ⬜ 未着手 |
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
- [x] サーバー: Pythonプロジェクト構造作成
- [x] サーバー: 各モジュールのスタブ作成

### 作成ファイル一覧

#### デバイス側（transceiver/）
```
main/
  main.c          起動シーケンス骨格
  config.h        全設定定数
  CMakeLists.txt  コンポーネント登録済み

components/
  pmic/    axp2101.c/h, led.c/h, CMakeLists.txt
  modem/   at_cmd.c/h, modem.c/h, CMakeLists.txt
  network/ ctrl_client.c/h, udp_client.c/h, CMakeLists.txt
  protocol/ protocol.c/h, floor_ctrl.c/h, CMakeLists.txt
  audio/   i2s_capture.c/h, i2s_playback.c/h, jitter_buf.c/h, CMakeLists.txt
  codec/   opus_codec.c/h, CMakeLists.txt
  state/   state_machine.c/h, CMakeLists.txt
```

#### サーバー側（server/）
```
main.py
config.py
protocol.py
device.py
floor.py
ctrl_server.py
audio_server.py
web_server.py
monitor.py
requirements.txt
```

---

## Step 2: PMIC・LED（AXP2101） ✅

（詳細省略）

---

## Step 3: モデム起動・LTE-M接続 ✅

（詳細省略）

---

## Step 4: リレーサーバー（Python）+ UDP制御接続（デバイス） ✅

### 完了タスク

- [x] サーバー: Python asyncio + websockets で実装
  - `ctrl_server.py`: UDP port 6000（制御チャンネル）
  - `audio_server.py`: UDP port 6001（音声中継）
  - `web_server.py`: WebSocket port 8080（モニターUI）
- [x] デバイス: 制御チャンネルを TCP → UDP に変更
  - `modem.h/c`: `modem_tcp_*` → `modem_ctrl_*`（"UDP" で CAOPEN）
  - `tcp_client.c/h` → `ctrl_client.c/h` に置き換え
  - `config.h`: `CONFIG_TCP_PORT` → `CONFIG_CTRL_PORT`
- [x] デバイス: HELLO/HELLO_ACK ハンドシェイク動作確認
- [x] サーバー: VPS (160.251.214.253) へデプロイ・稼働確認

### 制御チャンネルフロー（UDP）

```
ctrl_task
  → modem_ctrl_open(IP, 6000)    # AT+CAOPEN=0,0,"UDP"
  → send HELLO（device_id, group_id）
  ← recv HELLO_ACK（session_id）
  → state_set(EVT_CONNECTED)
  → 送受信ループ（500ms ポーリング）
```

---

## Step 5-1: PTT動作確認 ✅

### 完了タスク

- [x] PTT 押下で `EVT_FLOOR_GRANTED` がセット → LED が 4Hz 点滅に変化
- [x] PTT 解放で `EVT_FLOOR_GRANTED` がクリア → LED が常時点灯（待機）に戻る
- [x] サーバーモニターで端末ステータスが `transmitting` / `standby` と切り替わること

---

## Step 5-2: 音声送信確認 ✅

### 完了タスク

- [x] PTT 中にデバイスから UDP 音声パケットが継続送信される
- [x] サーバーがパケットを受信・中継する
- [x] モニター UI の音声モニターで音声が再生される（WebCodecs / Chrome）

---

## Step 6: 音声送信パイプライン ✅

### 完了タスク

- [x] `config.h`: I2S GPIO ピン定義追加、サンプルレート 16kHz・ビットレート 16kbps に設定
- [x] `state_machine.h`: `pcm_frame_t`（320サンプル, `CONFIG_OPUS_FRAME_SAMPLES`）, `encoded_frame_t`, キュー追加
- [x] `state_machine.c`: 音声キュー作成（`state_machine_init()`）
- [x] `i2s_capture.c`: INMP441実装（16kHz, 32bit slot, 16bit変換, MIC_GAIN_X=4）
- [x] `opus_codec.c`: エンコーダ実装（16kbps, complexity=5, FEC有効, `opus_encode_task`）
- [x] `modem.h/c`: `modem_udp_open()`, `modem_udp_send()`, `modem_udp_close()` 実装
- [x] `udp_client.c`: `udp_tx_task` 実装（EVT_CONNECTED待機、キープアライブ25s）

### 音声設定

| 項目 | 値 |
|------|----|
| サンプルレート | 16kHz |
| ビットレート | 16kbps |
| フレーム長 | 20ms（320サンプル） |
| エンコード品質 | complexity=5 |
| FEC | 有効（パケットロス想定 10%）|
| マイクゲイン | 4倍（INMP441 低感度補正）|

### 音声パイプライン（送信）

```
i2s_capture_task → [g_pcm_encode_queue] → opus_encode_task → [g_encoded_tx_queue] → udp_tx_task
```

### 関連バグ修正（Step 5〜6 作業中に対処）

- **UDP send: no prompt 交互失敗**: `uart_flush_input` + `pbuf[128]` + OK消費で解決
- **LTE 約7分後切断（PSM）**: `AT+CPSMS=0` + `AT+CEDRXS=0` で PSM 無効化
- **モデム自発リセット後の復旧**: `modem_ensure_network()` 追加、CAOPEN 連続失敗時に呼び出し
- **モニター音声が再生されない（WebCodecs 48kHz問題）**: ChromeのOpusデコーダが常に48kHz出力するため `audioData.sampleRate` を動的参照に修正

---

## Step 7-1: modem_udp_recv 実装 ⬜

### 目的

音声チャンネル（conn_id=1）からの受信パケット読み出し API を追加する。  
`modem_ctrl_recv` と同様の実装で、接続 ID のみ異なる。

### 実装内容

**`modem.h`** に追加:
```c
esp_err_t modem_udp_recv(uint8_t *buf, size_t max_len, size_t *out_len, uint32_t timeout_ms);
```

**`modem.c`** に追加:
- `AT+CARECV=1,max_len` を発行
- `+CARECV: N,<data>` レスポンスを解析
- データなし（N=0）時は `out_len=0` で `ESP_OK` を返す

### 合格条件

- [ ] `modem_udp_recv` がビルドエラーなくコンパイルできること

---

## Step 7-2: udp_rx_task 実装 ⬜

### 目的

サーバーから中継された音声 UDP パケットを受信し、ジッタバッファに投入する。

### 実装内容

**`udp_client.c`** の `udp_rx_task` スタブを実装:

```
udp_rx_task
  → EVT_CONNECTED 待機
  → ループ:
      modem_udp_recv(buf, sizeof(buf), &got, 20ms)
      got > 0 の場合:
        UDPヘッダー解析（6バイト: session_id, flags, seq, timestamp）
        UDP_TYPE_AUDIO かつ 自分以外のセッションのパケット:
          → jitter_buf_push(seq, timestamp, payload, payload_len)
        UDP_TYPE_KEEPALIVE: 無視
```

### 注意事項

- `modem_udp_recv` は `s_at_mutex` を取得するため、`ctrl_task` の recv と直列化される
- ポーリング間隔は 20ms（音声フレーム周期と同じ）に設定し、遅延を最小化
- `EVT_DISCONNECTED` 時はループを抜けて再待機

### 合格条件

- [ ] サーバーが音声を中継している状態で、`udp_rx_task` がパケットを受信するログが出ること
- [ ] ジッタバッファにフレームが積まれること

---

## Step 7-3: opus_decode_task 実装 ⬜

### 目的

ジッタバッファからフレームを取り出し Opus デコードして `g_pcm_playback_queue` に投入する。

### 実装内容

**`opus_codec.c`** の `opus_decode_task` スタブを実装:

```
opus_decode_task
  → Opus デコーダ初期化（16kHz, mono）
  → EVT_CONNECTED 待機
  → ループ（20ms 周期）:
      jitter_buf_pop(&frame)
      フレームあり:
        opus_decode(frame.data, frame.len, pcm, FRAME_SAMPLES)
        g_pcm_playback_queue に push
      フレームなし（途絶）:
        opus_decode(NULL, 0, pcm, FRAME_SAMPLES)  # PLC（パケットロス隠蔽）
        pcm が無音でなければ g_pcm_playback_queue に push
```

### 合格条件

- [ ] `g_pcm_playback_queue` にデコード済み PCM が流れること（ログで確認）

---

## Step 7-4: i2s_playback_task 実装 ⬜

### 目的

`g_pcm_playback_queue` から PCM を取り出し I2S DMA 経由でスピーカーへ出力する。

### 実装内容

**`i2s_playback.c`** の `i2s_playback_task` スタブを実装:

```
i2s_playback_task
  → I2S 初期化（16kHz, 32bit slot stereo, PCM5102向け設定）
  → ループ:
      g_pcm_playback_queue から pcm_frame_t を受信（待機）
      i2s_channel_write(pcm, samples * 2bytes)
```

### 合格条件

- [ ] スピーカーから音声が出ること（無音・ノイズなし）

---

## Step 7-5: エンドツーエンド動作確認 ⬜

### シナリオ

端末A（送話）と端末B（受話）の 2 台で確認する。

```
[端末A]                    [サーバー]              [端末B]
  PTT押下
  → PTT_START ────────────► floor grant
  ← PTT_START_ACK ──────── → PTT_NOTIFY ──────────► EVT_FLOOR_BUSY セット
  音声キャプチャ開始
  → UDP: Opusフレーム ─────► 中継 ─────────────────► jitter_buf_push
                                                      opus_decode
                                                      i2s_playback
  PTT解放
  → PTT_STOP ─────────────► floor release
                            → PTT_NOTIFY_STOP ───────► EVT_FLOOR_FREE
```

### 合格条件

- [ ] 端末A で PTT 押下中、端末B のスピーカーから端末A の音声が聞こえること
- [ ] 遅延が実用範囲内（目安: 500ms 以内）であること
- [ ] PTT 解放後、端末B の音声出力が止まること

---

## Step 8: エラー処理・再接続 ⬜

（Step 7-5 完了後に詳細化）

---

## Step 9: デバッグモニター ⬜

（Step 8 完了後に詳細化）
