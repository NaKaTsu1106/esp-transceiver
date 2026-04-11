# IPトランシーバー 仕様書 兼 設計書

**デバイス**: LilyGO T-SIM7080G-S3  
**作成日**: 2026-04-07  
**更新日**: 2026-04-11  
**バージョン**: 0.2.0

---

## 目次

1. [システム概要](#1-システム概要)
2. [システム構成](#2-システム構成)
3. [ハードウェア仕様](#3-ハードウェア仕様)
4. [ネットワーク・通信仕様](#4-ネットワーク通信仕様)
5. [プロトコル設計](#5-プロトコル設計)
6. [音声処理仕様](#6-音声処理仕様)
7. [グループ・チャンネル管理](#7-グループチャンネル管理)
8. [UI・操作仕様](#8-ui操作仕様)
9. [サーバー仕様](#9-サーバー仕様)
10. [マルチコア・マルチタスク設計](#10-マルチコアマルチタスク設計)
11. [起動シーケンス](#11-起動シーケンス)
12. [エラー処理・再接続設計](#12-エラー処理再接続設計)
13. [デバッグモニターアプリ設計](#13-デバッグモニターアプリ設計)
14. [システムアーキテクチャ](#14-システムアーキテクチャ)
15. [将来拡張](#15-将来拡張)

---

## 1. システム概要

### 1.1 目的

LilyGO T-SIM7080G-S3を用いたIPトランシーバーシステムを構築する。  
LTE-Mネットワークを介して複数端末間でリアルタイム音声通信（PTT方式）を実現する。

### 1.2 ユースケース

- 複数人グループでのチーム通信
- PTT（Push-to-Talk）方式による半二重音声通話
- 複数グループへの振り分けと切り替え

### 1.3 システム規模

| 項目 | 仕様 |
|------|------|
| 最大端末数 | 10台 |
| 最大グループ数 | 8グループ |
| 同時送話端末数 | 1台（グループ内） |

---

## 2. システム構成

### 2.1 全体アーキテクチャ

```
[端末A] ──LTE-M──┐
[端末B] ──LTE-M──┼──► [VPS 中継サーバー] ◄──LTE-M──[端末C]
[端末D] ──LTE-M──┘
```

- すべての端末はVPS中継サーバーを経由して通信する
- 端末同士の直接通信（P2P）は行わない
- サーバーはグループ単位で音声パケットを転送する

### 2.2 チャンネル構成

| チャンネル | プロトコル | 用途 |
|------------|------------|------|
| 制御チャンネル | UDP | PTT状態通知、端末管理、グループ切替 |
| 音声チャンネル | UDP | 音声データ転送 |

---

## 3. ハードウェア仕様

### 3.1 メインボード

| 項目 | 内容 |
|------|------|
| ボード名 | LilyGO T-SIM7080G-S3 |
| MCU | ESP32-S3（Xtensa 32-bit LX7 デュアルコア、最大240MHz） |
| モジュール | ESP32-S3-WROOM-1-N16R8 |
| フラッシュ | 16MB SPI Flash |
| PSRAM | 8MB Octal SPI PSRAM（OPI 80MHz、GPIO35〜37は予約済み・使用禁止） |
| 内蔵SRAM | 512KB |
| Wi-Fi / BT | 2.4GHz Wi-Fi 4、Bluetooth 5 LE（本プロジェクトでは未使用） |
| モデム | SIMCom SIM7080G |
| 対応通信規格 | LTE-M（Cat-M1）、NB-IoT |
| 使用通信規格 | LTE-M（Cat-M1）のみ |
| SIMカード | nanoSIM（SORACOM SIM） |
| 開発フレームワーク | ESP-IDF v6.0 |

#### PSRAM 設定

Opus コーデックの疑似スタック（エンコーダ 120KB + デコーダ 120KB = 計 240KB）を PSRAM に確保するため、
OPI PSRAM を有効化する。内部 HEAP のみでは 2 つの疑似スタックを同時確保できずクラッシュする。

| 設定 | 値 |
|------|----|
| `CONFIG_SPIRAM` | 有効 |
| `CONFIG_SPIRAM_MODE_OCT` | 有効（OPI 8MB） |
| `CONFIG_SPIRAM_SPEED_80M` | 有効（80MHz） |
| `CONFIG_SPIRAM_BOOT_INIT` | 有効（起動時初期化） |
| 割り当て方式 | `CAPS_ALLOC`（`malloc()` は内部RAMのまま、`heap_caps_malloc(MALLOC_CAP_SPIRAM)` でのみ PSRAM を使用） |

### 3.2 電源管理IC（PMIC）

#### AXP2101（X-Powers）

| 項目 | 内容 |
|------|------|
| IC型番 | AXP2101 |
| 通信インターフェース | I2C |
| I2Cアドレス | 0x34（7ビット）※ 8ビット表記では 0x68 |
| SDA | GPIO 15 |
| SCL | GPIO 7 |
| IRQ | GPIO 6 |

#### 主要電源出力

| 出力 | 電圧 | 用途 |
|------|------|------|
| DCDC1 | 3.3V固定 | システム電源 |
| DCDC3 | 3000mV | SIM7080G電源 |
| BLDO1 | 3300mV | レベルシフト用 **無効化禁止**（モデム↔ESP32間通信が停止する） |
| ALDO3 | 可変 | SDカード電源 |

#### チャージLED制御（状態インジケーター）

バッテリー充電機能は使用しない。AXP2101のチャージLEDを手動制御モードで**メインの状態インジケーター**として使用する。

| 項目 | 内容 |
|------|------|
| 制御レジスタ | 0x69 |
| 保持ビット | 0xC8（bit3、bit6〜7は予約のため変更禁止） |
| 手動制御有効化 | bit2=MAN=1、bit0=AUTO=1（`0x05` を OR する） |

**レジスタビットマップ**

| bit | 役割 |
|-----|------|
| 7-6 | 予約（変更禁止） |
| 5-4 | LED状態（手動制御時）: 00=OFF, 01=1Hz, 10=4Hz, 11=ON |
| 3 | 予約（変更禁止） |
| 2 | MAN（手動制御有効）= 1 に固定 |
| 1 | 予約（変更禁止） |
| 0 | AUTO（充電連動制御）= 1 に固定 |

**レジスタ操作手順**（Read-Modify-Write、XPowersLib `setChargingLedMode()` 準拠）

```c
// 1. レジスタ読み出し
uint8_t val = axp2101_read(0x69);
// 2. 予約ビット（bit3, 6, 7）のみ保持
val &= 0xC8;
// 3. 手動制御ビット（bit2=MAN, bit0=AUTO）をセット
val |= 0x05;
// 4. LED状態を bit5-4 に配置
val |= (led_mode << 4);
// 5. レジスタ書き込み
axp2101_write(0x69, val);
```

**bits\[5:4\] LED状態定義**（`led_mode` の値）

| bits\[5:4\] | 定数名 | 動作 | レジスタ書き込み値（予約bit=0の場合） |
|-------------|--------|------|--------------------------------------|
| `00`（0） | `CHGLED_OFF` | 消灯 | `0x05` |
| `01`（1） | `CHGLED_1HZ` | 1Hz点滅 | `0x15` |
| `10`（2） | `CHGLED_4HZ` | 4Hz点滅 | `0x25` |
| `11`（3） | `CHGLED_ON` | 常時点灯 | `0x35` |

> **注意**: AXP2101に2Hzモードは存在しない。

#### バッテリー・充電

| 項目 | 内容 |
|------|------|
| 対応バッテリー | 18650 リチウムイオン（フラットトップ推奨、2500mAh以上） |
| 充電電流 | USB-C経由（デフォルト設定のまま使用） |
| USB充電 | USB-C（プログラミング兼用） |

### 3.3 SIM7080Gモデムとの通信インターフェース

#### UART接続

| 項目 | 内容 |
|------|------|
| インターフェース | UART2 |
| RXD | GPIO 4 |
| TXD | GPIO 5 |
| ボーレート | 921600 bps（通常）/ 115200 bps（フォールバック） |
| 電圧レベル | 3.3V TTL |

#### モデム制御ピン

| ピン名 | GPIO | 機能 |
|--------|------|------|
| PWR | GPIO 41 | 電源制御（トランジスタ反転: HIGH=PWRKEY アサート） |
| DTR | GPIO 42 | Data Terminal Ready |
| RI | GPIO 3 | Ring Indicator |

#### 注意事項

- SIMカードは**電源投入前に挿入**すること（起動後の挿入は認識されない）
- **セルラー通信とGNSSの同時使用は不可**
- BLDO1（レベルシフタ電源）を無効化するとモデムとの通信が停止する

### 3.4 音声入出力

#### 試作フェーズ（現フェーズ）

| 項目 | 内容 |
|------|------|
| マイク | INMP441（I2S接続 MEMSマイク） |
| スピーカー出力 | PCM5102（I2S DAC）→ 3.5mmジャック経由 |
| PTT | Icomインカムの2.5mmジャック Ring端子 → GPIO 0 |

#### 将来フェーズ

| 項目 | 内容 |
|------|------|
| オーディオコーデック | ES8311（I2S接続、ADC+DAC一体型） |
| マイク入力 | 2.5mmジャック Tip端子 → ES8311マイクプリアンプ入力 |
| スピーカー出力 | ES8311 DAC出力 → 3.5mmジャック |
| PTT | 2.5mmジャック Ring端子 → GPIO |

### 3.5 操作部（現フェーズ）

| 項目 | 内容 |
|------|------|
| PTTボタン | Icomインカム内蔵PTTスイッチ（2.5mmジャック Ring端子 → GPIO 0） |
| グループ切替 | 現フェーズはなし。グループIDは `config.h` に定数として書き込んだ固定値を使用 |
| LED（状態表示） | AXP2101チャージLED（I2Cレジスタ0x69で手動制御）。詳細はセクション8.2参照 |

### 3.6 GPIO割り当て

#### 使用済みGPIO（ボード固定）

| GPIO | 用途 |
|------|------|
| 3 | SIM7080G RI |
| 4 | SIM7080G RXD（UART2） |
| 5 | SIM7080G TXD（UART2） |
| 6 | AXP2101 IRQ |
| 7 | AXP2101 SCL（I2C） |
| 15 | AXP2101 SDA（I2C） |
| 35〜37 | PSRAM（使用禁止） |
| 38 | SDカード CLK |
| 39 | SDカード CMD |
| 40 | SDカード DATA |
| 41 | SIM7080G PWR |
| 42 | SIM7080G DTR |

#### アプリケーション用GPIO

| GPIO | 用途 |
|------|------|
| 0 | PTTボタン入力（Icomインカム Ring端子、LOW=押下） |
| 9 | I2S RX DIN（INMP441マイク データ） |
| 10 | I2S RX WS（INMP441マイク ワードセレクト） |
| 11 | I2S RX BCK（INMP441マイク ビットクロック） |
| 16 | I2S TX DOUT（PCM5102 DAC データ） |
| 17 | I2S TX WS（PCM5102 DAC ワードセレクト） |
| 18 | I2S TX BCK（PCM5102 DAC ビットクロック） |

---

## 4. ネットワーク・通信仕様

### 4.1 LTE-M接続

| 項目 | 内容 |
|------|------|
| 通信規格 | LTE-M（Cat-M1）のみ |
| SIMカード | SORACOM SIM（nanoSIM） |
| 接続方式 | AT コマンド経由（SIM7080G） |
| IPバージョン | IPv4 |
| NAT方式 | SORACOM NAPT経由（端末はプライベートIP、公開IPは共有） |

### 4.2 SORACOM SIM 設定

#### APN・認証設定

| 項目 | 値 |
|------|-----|
| APN | `soracom.io` |
| ユーザー名 | `sora` |
| パスワード | `sora` |
| 認証方式 | PAP（Authentication Protocol = 1） |
| PDPタイプ | IP（IPv4） |

#### SIM7080G ATコマンド設定手順

起動シーケンスPhase 3〜4で以下の順に実行する。

```
// 通信規格をLTE-M（Cat-M1）のみに限定
AT+CNMP=38        → OK   // LTE Onlyモード
AT+CMNB=1         → OK   // Cat-M1のみ（NB-IoT無効）

// PDPコンテキスト設定（APN）
AT+CGDCONT=1,"IP","soracom.io"  → OK

// 認証設定（PAP: type=1）
AT+CGAUTH=1,1,"sora","sora"     → OK

// PSM（Power Saving Mode）無効化 ← 常時受信待機のため必須
AT+CPSMS=0        → OK

// eDRX（Extended Discontinuous Reception）無効化 ← 受信遅延防止
AT+CEDRXS=0       → OK

// ネットワーク登録待機
AT+CEREG?         → +CEREG: 0,1  // 0,1=登録済み / 0,5=ローミング登録済み

// GPRS アタッチ確認
AT+CGATT=1        → OK

// PDPコンテキスト有効化
AT+CGACT=1,1      → OK

// サーバー返却APNの確認（接続安定性向上）
AT+CGNAPN         → OK

// アプリネットワーク設定
AT+CNCFG=0,1,"soracom.io","sora","sora",1  → OK

// アプリネットワーク有効化（+APP PDP: 0,ACTIVE URC を待つ）
AT+CNACT=0,1      → OK  → URC: +APP PDP: 0,ACTIVE

// IPアドレス確認
AT+CGPADDR=1      → +CGPADDR: 1,10.x.x.x

// 電波強度確認（デバッグ用）
AT+CSQ            → +CSQ: <rssi>,<ber>
```

#### 電波強度（AT+CSQ）の読み方

| RSSI値 | 電波強度 | 目安 |
|--------|---------|------|
| 0〜9 | 非常に弱い | 通信困難 |
| 10〜14 | 弱い | 不安定 |
| 15〜19 | 普通 | 実用可 |
| 20〜30 | 良好 | 安定 |
| 31 | 最大 | 最良 |
| 99 | 不明 | 未接続 |

### 4.3 NAPT考慮事項

SORACOM SIMはNAPT（Network Address Port Translation）経由でインターネットに接続する。  
セッション切断時に送信元ポート番号が変化するため、以下の設計上の対策を行う。

| 対象 | 問題 | 対策 |
|------|------|------|
| UDP（制御） | 無音時にNAPTマッピングが消える | HEARTBEATをNAPTタイムアウト（約30秒）以内の間隔で定期送信してマッピングを維持する |
| UDP（制御） | 送信元ポートが変わる | サーバーはHELLO/HEARTBEATの送信元IP:Portを都度上書き更新し、常に最新アドレスへ制御メッセージを返送する |
| UDP（音声） | 無音時にNAPTマッピングが消える | 無音時もUDPキープアライブパケットを定期送信してマッピングを維持する |
| UDP（音声） | 送信元ポートが変わる | サーバーは受信パケットの送信元IP:ポートを都度更新し、常に最新アドレスへ転送する |

### 4.4 ポート一覧

| 用途 | プロトコル | ポート番号 |
|------|------------|------------|
| 制御チャンネル | UDP | 6000 |
| 音声チャンネル | UDP | 6001 |
| WebSocketモニター | TCP | 8080 |

---

## 5. プロトコル設計

### 5.1 制御チャンネル（UDP）

端末とサーバー間の制御メッセージをUDPで送受信する（ポート6000）。  
UDPは到達保証がないが、制御メッセージは小サイズかつHEARTBEATによる疎通監視を行うため、アプリケーションレベルの再送は実装しない。

#### 5.1.1 メッセージ形式

```
[1バイト: ペイロード長][1バイト: メッセージタイプ][ペイロード]
```

ペイロード長はペイロード部分のみのバイト数（メッセージタイプは含まない）。  
最大ペイロード長は255バイト（全メッセージが十分に収まる）。  
1つのUDPパケットに1つの制御メッセージを格納する（分割・結合なし）。

#### 5.1.2 メッセージタイプ一覧

| タイプ値 | 名称 | 方向 | 内容 |
|----------|------|------|------|
| 0x01 | HELLO | 端末→サーバー | 接続要求（端末ID=MACアドレス下4バイト、グループID） |
| 0x02 | HELLO_ACK | サーバー→端末 | 接続承認 + セッションID割り当て（1バイト、以降のUDPで使用） |
| 0x03 | PTT_START | 端末→サーバー | PTT押下（送話開始要求） |
| 0x04 | PTT_START_ACK | サーバー→端末 | 送話権許可 |
| 0x05 | PTT_START_DENY | サーバー→端末 | 送話権拒否（他端末が送話中） |
| 0x06 | PTT_STOP | 端末→サーバー | PTT解放（送話終了） |
| 0x07 | PTT_NOTIFY | サーバー→端末 | 他端末が送話開始したことを通知（ペイロード: 送話元セッションID 1バイト） |
| 0x08 | PTT_NOTIFY_STOP | サーバー→端末 | 他端末が送話終了したことを通知 |
| 0x09 | GROUP_CHANGE | 端末→サーバー | グループ切替要求 |
| 0x0A | GROUP_CHANGE_ACK | サーバー→端末 | グループ切替承認 |
| 0x0B | HEARTBEAT | 端末→サーバー | 接続維持（keepalive）。端末が25秒間隔で送信。サーバーは75秒以内に届かなければ切断とみなす |
| 0x0C | DISCONNECT | 端末→サーバー | 切断通知 |

### 5.2 音声チャンネル（UDP）

音声データをUDPパケットで送受信する。

#### 5.2.1 UDPパケット形式

パケットサイズを最小化するため、フィールドをビットレベルで詰める。  
端末IDはサーバーが制御UDP HELLO_ACKで割り当てる1バイトのセッションIDを使用する。

```
[1バイト: セッションID][1バイト: フラグ][2バイト: シーケンス番号][2バイト: タイムスタンプ][2バイト: Opusペイロード長][Nバイト: ペイロード]
```

**ヘッダー合計: 8バイト**

| フィールド | サイズ | 内容 |
|------------|--------|------|
| セッションID | 1バイト | サーバー割り当て（1〜10）。HELLO_ACKで通知 |
| フラグ | 1バイト | bit7=パケットタイプ、bit6〜4=予約(000)、bit3〜0=グループID(1〜8) |
| シーケンス番号 | 2バイト | パケット順序管理・欠落検知（uint16 big-endian、ラップアラウンドあり） |
| タイムスタンプ | 2バイト | 送信時刻（ms、相対値、uint16 big-endian、ラップアラウンドあり） |
| Opusペイロード長 | 2バイト | Opusフレームのバイト数（uint16 big-endian）。キープアライブ時は 0 |
| ペイロード | 可変 | 音声時: Opusフレーム、キープアライブ時: なし（0バイト） |

**フラグバイトのビットレイアウト**

```
bit7    bit6-4    bit3-0
 │        │         │
 │      予約(000)   グループID (1〜8)
 │
 0 = UDP_TYPE_AUDIO      (音声データ)
 1 = UDP_TYPE_KEEPALIVE  (NAPTマッピング維持)
```

#### 5.2.2 パケットサイズの目安

| パケット種別 | サイズ |
|-------------|--------|
| キープアライブ | **8バイト** |
| 音声（Opus 16kbps / 20msフレーム） | **8 + 約40 = 約48バイト** |

#### 5.2.3 サーバーの転送動作

- **受信時に必ず**送信元IP:Portを端末ごとに記録・更新する（NAPT対応）
- `UDP_TYPE_AUDIO`: 送話権を持つ端末からのみ受け付け、同一グループの他端末へ転送
- `UDP_TYPE_KEEPALIVE`: 転送しない。送信元アドレスの更新のみ行う
- 送話権を持たない端末からの `UDP_TYPE_AUDIO` は破棄する

#### 5.2.4 NAPTアドレス動的更新（サーバー側）

SORACOMのNAPTによりデバイスの送信元ポートはセッション切断ごとに変化する。  
サーバーはUDPパケット受信のたびに送信元アドレスを上書き更新し、常に最新のアドレスへ転送する。

#### 5.2.5 キープアライブ送信（デバイス側）

NAPTマッピングを維持するため、デバイスは常にUDPを定期送信する必要がある。  
音声パケットの送信がない状態（待機中・受話中）では `UDP_TYPE_KEEPALIVE` を送信する。

| 状態 | UDP送信内容 | 間隔 |
|------|------------|------|
| 送話中 | `UDP_TYPE_AUDIO`（音声パケット） | 20ms |
| 待機中・受話中 | `UDP_TYPE_KEEPALIVE` | 25秒 |

---

## 6. 音声処理仕様

### 6.1 コーデック

| 項目 | 内容 |
|------|------|
| コーデック | Opus |
| ライブラリ | esp-libopus（ESP-IDF向けOpusポート、PSRAM対応疑似スタック） |
| サンプリングレート | 16kHz |
| チャンネル | モノラル |
| ビットレート | 16 kbps |
| フレームサイズ | 20ms（320サンプル） |
| 用途モード | VOIP |
| complexity | 5 |
| Inband FEC | 有効（パケットロス時の前方誤り訂正） |
| パケットロス想定 | 5% |

#### Opusメモリ仕様（PSRAM使用）

| 項目 | 内容 |
|------|------|
| 疑似スタック方式 | `NONTHREADSAFE_PSEUDOSTACK`（スレッドローカルストレージ経由で擬似的にスレッドセーフ化） |
| 疑似スタックサイズ | 120KB / スレッド |
| 確保先 | PSRAM（`heap_caps_malloc(MALLOC_CAP_SPIRAM)`）。失敗時は内部RAMにフォールバック |
| エンコーダ疑似スタック | 120KB（PSRAM） |
| デコーダ疑似スタック | 120KB（PSRAM）。タスク起動直後に PLC ウォームアップ呼び出しで確保 |

### 6.2 送受信フロー

**送信側（PTT押下時）**

```
マイク入力 → PCMキャプチャ → Opusエンコード → UDPパケット化 → サーバーへ送信
```

**受信側**

```
サーバーからUDP受信 → ジッタバッファ → Opusデコード → PCM出力 → スピーカー再生
```

### 6.3 送話権制御（Floor Control）

- PTTボタン押下時にサーバーへ `PTT_START` を送信
- サーバーがグループ内の送話権を管理し、空きの場合のみ `PTT_START_ACK` を返す
- 送話権を得た端末のみが音声UDPパケットを送信する（`EVT_FLOOR_GRANTED` ビットで制御）
- PTTボタン解放時に `PTT_STOP` を送信し送話権を解放する
- **PTT_START_ACK の遅延受信処理**: ACK が PTT ボタン解放より後に届いた場合は `EVT_FLOOR_GRANTED` をセットせずに破棄する（`EVT_PTT_PRESSED` が既にクリアされているか確認する）

---

## 7. グループ・チャンネル管理

### 7.1 グループ仕様

| 項目 | 内容 |
|------|------|
| 最大グループ数 | 8 |
| グループID | 1〜8（1バイト） |
| 同時送話 | グループ内1端末のみ |
| グループ間の通信 | なし（グループは独立） |

### 7.2 端末のグループ所属

- 各端末は同時に1つのグループにのみ所属する
- グループ切替は制御チャンネル（UDP）経由で行う
- 切替中は音声送受信を停止する

### 7.3 端末管理

| 項目 | 内容 |
|------|------|
| 最大端末数 | 10台 |
| 端末識別子 | ESP32-S3のWi-Fi MACアドレス下4バイト（32bit） |
| 端末状態 | 接続中 / 待機中 / 送話中 |

---

## 8. UI・操作仕様

### 8.1 現フェーズ（ボタンのみ）

| 操作 | 内容 |
|------|------|
| PTTボタン押下 | 送話開始 |
| PTTボタン解放 | 送話終了 |
| グループ切替 | 現フェーズはなし（config.h 固定値） |

### 8.2 状態表示（LED）

AXP2101のチャージLEDをI2C（レジスタ0x69）で手動制御する。  
`led_task`がイベントグループを監視し、状態変化に応じてレジスタを更新する。

| システム状態 | LED動作 | レジスタ値 |
|-------------|---------|-----------|
| 初期化中（PMIC設定まで） | 消灯（制御不可） | — |
| 起動シーケンス中（モデム起動〜タスク起動） | 1Hz点滅 | `0x15` |
| 待機中（全接続完了、PTT待ち） | 常時点灯 | `0x35` |
| 受話中（他端末が送話中） | 1Hz点滅 | `0x15` |
| 送話中（PTT押下、送話権取得済み） | 4Hz点滅 | `0x25` |
| 接続エラー・再接続中 | 1Hz点滅 | `0x15` |
| SIM未検出（停止） | 消灯 | `0x05` |

### 8.3 将来フェーズ（ディスプレイ追加時）

- 現在のグループ番号表示
- 送話中の端末ID表示
- 電波強度・接続状態表示
- バッテリー残量表示

---

## 9. サーバー仕様

### 9.1 環境

| 項目 | 内容 |
|------|------|
| 種別 | 一般的なVPS |
| OS | Debian |
| 実装言語 | Python 3.11+（asyncio） |
| 主要依存ライブラリ | `websockets`（WebSocketサーバー）|
| モニタリングUI | HTML + JavaScript（デバッグ用途、WebSocket受信） |

### 9.2 サーバーの役割

- 端末からの制御UDP（ポート6000）を受け付け、認証・管理を行う
- グループごとに送話権（Floor Control）を管理する
- 音声UDPパケット（ポート6001）を同一グループの他端末へ転送する
- HEARTBEATにより端末の接続状態を監視する
- WebSocket（ポート8080）でデバッグモニタリングUIを提供する
- ループバック機能（テスト用）: PTT音声を録音し、PTT終了後に送信元へ折り返す

### 9.3 サーバー処理フロー

```
起動
├─ UDP制御リスン開始（ポート6000）
├─ UDP音声リスン開始（ポート6001）
├─ WebSocketサーバー起動（ポート8080）
└─ 接続待機

端末接続時（制御UDP）
├─ HELLO受信 → 端末登録、グループ割当、送信元IP:Port記録
└─ HELLO_ACK送信（記録したIP:Portへ返送）

PTT_START受信時
├─ グループ内送話権が空き → PTT_START_ACK送信 + 他端末へPTT_NOTIFY送信
└─ 送話権が使用中 → PTT_START_DENY送信

音声UDP受信時
├─ 送信元IP:Portを端末テーブルに上書き更新（NAPT対応・全パケット共通）
├─ UDP_TYPE_KEEPALIVEパケット → アドレス更新のみ、転送しない
├─ UDP_TYPE_AUDIOかつ送話権保持端末かつループバックOFF → 同グループ他端末へ転送
├─ UDP_TYPE_AUDIOかつ送話権保持端末かつループバックON → バッファへ蓄積
└─ UDP_TYPE_AUDIOかつ送話権なし端末 → 破棄

PTT_STOP受信時
├─ 送話権解放
├─ 他端末へPTT_NOTIFY_STOP送信
└─ ループバックON時: バッファを折り返し送信（PTT_NOTIFY(0xFF) → 音声パケット再送 → PTT_NOTIFY_STOP）

タイムアウト監視（定期タスク・10秒間隔）
└─ 最終受信から75秒以上経過した端末 → セッション削除 + 送話権解放
```

### 9.4 Pythonモジュール構成

```
server/
├─ main.py               # エントリポイント。asyncio.run() でサーバー群を起動
├─ config.py             # 設定（環境変数ロード）
├─ protocol.py           # メッセージ型定数・エンコード/デコード関数
├─ device.py             # 端末セッション管理（辞書、asyncioシングルスレッドで排他）
├─ floor.py              # グループ単位の送話権（Floor Control）管理
├─ ctrl_server.py        # UDP制御サーバー（asyncio.DatagramProtocol）
├─ audio_server.py       # UDP音声サーバー（asyncio.DatagramProtocol）
├─ loopback.py           # ループバック機能（音声バッファ・折り返し送信）
├─ monitor.py            # WebSocketブロードキャスト管理・ログ管理
└─ web_server.py         # WebSocketサーバー + 静的ファイル配信（HTTP）
```

#### asyncio構成

すべてのサーバーは単一の asyncio イベントループ上で動作する。  
スレッドは使用しない。共有状態へのアクセスはコルーチン内（シングルスレッド）で行うため、GILによる排他が得られる。

```
asyncio.run(main())
 ├─ loop.create_datagram_endpoint(CtrlProtocol, ...)   # UDP :6000
 ├─ loop.create_datagram_endpoint(AudioProtocol, ...)  # UDP :6001
 ├─ asyncio.create_task(reaper_task())                 # タイムアウト監視
 └─ websockets.serve(ws_handler, "0.0.0.0", 8080)     # WebSocket
```

#### ループバック機能（loopback.py）

| 項目 | 内容 |
|------|------|
| 用途 | 単体テスト。1台の端末でPTT → 自分の声が折り返し再生される |
| サーバー仮想セッションID | `0xFF`（端末の自己パケットフィルタを回避するための予約値） |
| 最大バッファ | 6000パケット（約120秒分） |
| 折り返し動作 | PTT_NOTIFY(0xFF) 送信 → 20ms 間隔でパケット再送 → PTT_NOTIFY_STOP 送信 |
| 有効化 | WebSocket モニターの「ループバック」トグルで切り替え |

### 9.5 WebSocketモニタリングAPI

WebSocketサーバーはポート8080で待ち受ける。

| エンドポイント | プロトコル | 内容 |
|---------------|-----------|------|
| `ws://<host>:8080/` | WebSocket | 制御イベント・ログ・端末状態・音声フレームの双方向通信 |
| `http://<host>:8080/` | HTTP | 静的ファイル配信（`web/static/`） |

#### 接続時の初期配信

WebSocket接続確立直後に、サーバーは以下を順番に送信する。

```
1. snapshot イベント（現在の全端末状態）
2. 直近のログエントリ（最大 LOG_MAX_ENTRIES 件）
3. audio_init イベント（音声デコーダ初期化情報）
4. loopback_state イベント（現在のループバック有効/無効状態）
```

#### サーバー→クライアント イベント形式（JSON）

| `type` 値 | タイミング | 主なフィールド |
|-----------|-----------|--------------|
| `"snapshot"` | WebSocket接続時 | `devices` 配列（全端末の現在状態） |
| `"devices"` | 端末状態変化時 | `devices` 配列（session_id, device_id, group_id, status, connected_at, last_seen） |
| `"log"` | 各種制御イベント時 | `level`, `device_id`, `message`, `timestamp` |
| `"audio_init"` | WebSocket接続時 | `sample_rate`=16000, `channels`=1 |
| `"audio"` | 音声UDPパケット中継時（サブスクライブ済みの場合） | `group`, `session`, `seq`, `ts`, `data`（base64 Opus） |
| `"loopback_state"` | WebSocket接続時・切替時 | `enabled`（true/false） |

#### クライアント→サーバー メッセージ形式（JSON）

| `action` 値 | 内容 |
|------------|------|
| `"subscribe_audio"` | 音声フレームの配信を開始する |
| `"unsubscribe_audio"` | 音声フレームの配信を停止する |
| `"set_loopback"` | ループバック有効/無効を切り替える（`enabled`: true/false） |

音声フレームはデータ量が多いため、明示的にサブスクライブした接続にのみ送信する。

### 9.6 設定（環境変数）

| 環境変数 | デフォルト値 | 内容 |
|---------|------------|------|
| `CTRL_PORT` | `6000` | UDP制御ポート |
| `UDP_PORT` | `6001` | UDP音声ポート |
| `WS_PORT` | `8080` | WebSocketモニターポート |
| `HEARTBEAT_TIMEOUT` | `75` | セッションタイムアウト（秒） |
| `LOG_MAX_ENTRIES` | `1000` | メモリ保持ログの最大件数 |
| `LOG_DIR` | `/var/log/transceiver` | ログファイル保存ディレクトリ（空文字で無効） |
| `DOMAIN` | `` | HTTPS用ドメイン名（空文字でHTTP動作） |

---

## 10. マルチコア・マルチタスク設計

### 10.1 設計方針

ESP32-S3はデュアルコア（Core 0 / Core 1）を持つ。音声系と通信系を**物理的に異なるコアに分離**することで、処理の干渉を防ぎ動作を安定させる。

- **Core 0（PRO_CPU）**: 通信系専用（モデム・制御UDP・音声UDP・ハートビート）
- **Core 1（APP_CPU）**: 音声系専用（I2S・Opusエンコード/デコード・PTT・状態管理）

タスク間のデータ受け渡しはFreeRTOSのキューとイベントグループのみで行い、共有メモリへの直接アクセスは禁止する。

### 10.2 コア割り当てとタスク一覧

#### Core 0（PRO_CPU）— 通信系

| タスク名 | 優先度 | スタック | 役割 |
|----------|--------|----------|------|
| `ctrl_task` | 5 | 8KB | 制御チャンネル（UDP）の送受信、再接続処理 |
| `udp_rx_task` | 18 | 4KB | 音声UDPパケット受信 → ジッタバッファへ投入 |
| `udp_tx_task` | 15 | 4KB | 送信キューからパケット取り出し → UDP送信 |
| `heartbeat_task` | 3 | 2KB | 25秒ごとにHEARTBEATを制御UDPへ送信 |

#### Core 1（APP_CPU）— 音声系

| タスク名 | 優先度 | スタック | 役割 |
|----------|--------|----------|------|
| `i2s_capture_task` | 19 | 4KB | I2SからPCMを取得（INMP441） → エンコードキューへ投入 |
| `i2s_playback_task` | 19 | 4KB | 再生キューからPCMを取得 → I2Sで出力（PCM5102） |
| `opus_encode_task` | 20 | 8KB | PCMをOpusエンコード → UDP送信キューへ投入 |
| `opus_decode_task` | 20 | 8KB | 受信キューからOpusを取得 → デコード → 再生キューへ |
| `ptt_task` | 6 | 2KB | PTTボタンのGPIO監視、チャタリング除去（20ms）、状態通知 |
| `state_machine_task` | 4 | 4KB | 制御メッセージに基づきシステム状態を管理 |
| `led_task` | 3 | 2KB | イベントグループを監視し、AXP2101レジスタ0x69をI2C経由で更新してLED状態を制御 |

> 優先度はESP-IDF FreeRTOS基準（数値が大きいほど高優先）

### 10.3 タスク間通信

#### FreeRTOSキュー一覧

| キュー名 | 送信元 | 受信先 | アイテム | キュー深さ |
|----------|--------|--------|----------|-----------|
| `pcm_encode_queue` | `i2s_capture_task` | `opus_encode_task` | PCMフレーム（20ms / 320サンプル） | 4 |
| `encoded_tx_queue` | `opus_encode_task` | `udp_tx_task` | Opusパケット（seq・timestamp付き） | 4 |
| `received_rx_queue` | `udp_rx_task` | `opus_decode_task` | Opusパケット（ジッタバッファ） | 8 |
| `pcm_playback_queue` | `opus_decode_task` | `i2s_playback_task` | PCMフレーム（20ms / 320サンプル） | 4 |
| `ctrl_tx_queue` | 各タスク | `ctrl_task` | 制御メッセージ構造体 | 8 |
| `ctrl_rx_queue` | `ctrl_task` | `state_machine_task` | 制御メッセージ構造体 | 8 |

#### FreeRTOSイベントグループ（`g_system_events`）

| ビット | 名称 | 意味 |
|--------|------|------|
| Bit 0 | `EVT_MODEM_READY` | モデム初期化完了 |
| Bit 1 | `EVT_CONNECTED` | サーバーへの制御UDP接続確立（HELLO_ACK受信済み） |
| Bit 2 | `EVT_PTT_PRESSED` | PTTボタン押下中 |
| Bit 3 | `EVT_FLOOR_GRANTED` | 送話権取得済み |
| Bit 4 | `EVT_FLOOR_BUSY` | 他端末が送話中（受話中） |
| Bit 5 | `EVT_FLOOR_FREE` | 送話権が空き状態 |
| Bit 6 | `EVT_DISCONNECTED` | サーバー切断 |

### 10.4 音声パイプライン

#### 送信パイプライン（PTT押下 → 音声送信）

```
[I2S DMA割り込み]
      ↓
i2s_capture_task (Core1, 優先度19)  ← EVT_FLOOR_GRANTED 保持中のみキューへ投入
      ↓ pcm_encode_queue
opus_encode_task (Core1, 優先度20)
      ↓ encoded_tx_queue
udp_tx_task (Core0, 優先度15)
      ↓
[UDP送信 → VPSサーバー]
```

#### 受信パイプライン（音声受信 → スピーカー出力）

```
[VPSサーバー → UDP受信]
      ↓
udp_rx_task (Core0, 優先度18)
      ↓ received_rx_queue（ジッタバッファ、深さ8）
opus_decode_task (Core1, 優先度20)  ← 3フレーム以上溜まってから再生開始
      ↓ pcm_playback_queue
i2s_playback_task (Core1, 優先度19)
      ↓
[I2S DMA出力]
```

### 10.5 PTT制御フロー

```
ptt_task: PTTボタン押下検知（チャタリング除去: 20ms）
      ↓ EVT_PTT_PRESSED をイベントグループにセット
      ↓ MSG_PTT_START を ctrl_tx_queue へ投入
ctrl_task: PTT_START をサーバーへ送信
      ↓ PTT_START_ACK 受信
ctrl_task: ctrl_rx_queue へ ACK を投入
      ↓
state_machine_task: EVT_PTT_PRESSED が立っている場合のみ EVT_FLOOR_GRANTED をセット
      ↓                ※ ACK 遅延時（PTT解放後にACKが届いた場合）はセットしない
i2s_capture_task: EVT_FLOOR_GRANTED を確認してキャプチャ・エンコード開始

--- PTTボタン解放 ---

ptt_task: ボタン解放検知（チャタリング除去: 20ms）
      ↓ EVT_PTT_PRESSED・EVT_FLOOR_GRANTED をクリア
      ↓ MSG_PTT_STOP を ctrl_tx_queue へ投入
i2s_capture_task: キャプチャ停止（EVT_FLOOR_GRANTED なしのためキューへ投入しない）
ctrl_task: PTT_STOP をサーバーへ送信
```

### 10.6 ジッタバッファ

ネットワーク遅延のゆらぎ（ジッタ）による音飛びを防ぐため、受信側にジッタバッファを設ける。

| 項目 | 内容 |
|------|------|
| 実装 | `received_rx_queue`（深さ8、約160ms分） |
| 再生開始条件 | キューに3フレーム以上たまってから再生開始 |
| パケット欠落時 | Opusのパケットロス隠蔽（PLC）機能で補間。最大 25フレーム（500ms）まで継続 |
| PLC超過時 | 送信終了とみなし、次のフレーム到着まで待機 |
| キューあふれ時 | 古いフレームを破棄（遅延蓄積を防ぐ） |

### 10.7 モデム通信の分離

SIM7080GへのATコマンドはUART2経由で行うが、`modem`コンポーネントが排他的に管理する。他のタスクがUARTに直接アクセスすることは禁止する。

```
ctrl_task / udp_tx_task → modem_ctrl_open/send/recv/close()
                               ↓ Mutex保護ATコマンド（UART2、at_cmd.c）
                          SIM7080G モデム
```

---

## 11. 起動シーケンス

### 11.1 全体フロー概要

```
電源投入
  │
  ▼
[Phase 1] ESP32-S3 初期化
  │
  ▼
[Phase 2] AXP2101 PMIC 設定
  │
  ▼
[Phase 3] SIM7080G モデム起動
  │
  ▼
[Phase 4] LTE-M ネットワーク接続
  │
  ▼
[Phase 5] VPSサーバー接続（UDP ctrl + UDP audio）
  │
  ▼
[Phase 6] I2S 音声初期化
  │
  ▼
[Phase 7] FreeRTOS タスク起動
  │
  ▼
[待機状態] PTT待ち（LED: 常時点灯）
```

各フェーズで失敗した場合はLEDで状態を表示し、リトライまたは停止する。

---

### 11.2 Phase 1: ESP32-S3 初期化

`app_main()` から順番に実行する。

| ステップ | 処理 | 失敗時 |
|----------|------|--------|
| 1-1 | `config.h` から設定定数を読み込み（グループID・サーバーIP・ポート番号） | — |
| 1-2 | FreeRTOSイベントグループ・キュー生成 | 再起動 |

---

### 11.3 Phase 2: AXP2101 PMIC 設定

I2C経由（7ビットアドレス 0x34）でPMICを設定する。

| ステップ | 処理 | 備考 |
|----------|------|------|
| 2-1 | AXP2101の存在確認（チップID: 0x4A） | 失敗→再起動 |
| 2-2 | BLDO1 電圧設定（3300mV）→ 有効化 | **必須。無効化禁止**。電圧設定は有効化より前に行う |
| 2-3 | DCDC3 コールドスタート → 電圧設定（3000mV）→ 有効化 | すでにONの場合は200ms OFF後に電圧設定して再投入 |
| 2-4 | 100ms待機 | モデム電源安定待ち |
| 2-5 | LED を 1Hz 点滅に設定 | 起動シーケンス中を示す |

---

### 11.4 Phase 3: SIM7080G モデム起動

| ステップ | 処理 | タイムアウト | 失敗時 |
|----------|------|------------|--------|
| 3-0 | PWRKEY / DTR GPIO を OUTPUT LOW に初期化 | — | GPIO が浮遊のままだとトランジスタ経由で PWRKEY が不定アサートされブート不可 |
| 3-1 | AT疎通確認（1秒）でモデムが起動済みか確認 | 1秒 | 起動済みなら 3-2 をスキップ（2重トグル防止） |
| 3-2 | PWRKEYパルス: GPIO LOW(100ms) → HIGH(1000ms) → LOW → 2000ms待機 | — | トランジスタ反転: GPIO HIGH = PWRKEY アサート（≥1.3sでON/OFFトグル） |
| 3-3 | **921600 bpsで疎通確認**（`AT` → `OK`） | 3秒 | ステップ3-4へ（フォールバック） |
| 3-4 | （3-3失敗時）**115200 bpsに切替えて再確認** | 5秒 | 3回リトライ後に再起動 |
| 3-5 | （3-4成功時）`AT+IPR=921600` で921600 bpsへ変更 | 3秒 | 115200 bpsのまま続行 |
| 3-6 | エコーバック無効（`ATE0`） | 2秒 | 無視して続行 |
| 3-7 | PSM（Power Saving Mode）無効化（`AT+CPSMS=0`） | 3秒 | 警告ログのみ（非対応モデムあり） |
| 3-8 | eDRX（Extended Discontinuous Reception）無効化（`AT+CEDRXS=0`） | 3秒 | 警告ログのみ |
| 3-9 | SIM認識確認（`AT+CPIN?` → `READY`） | 5秒 | LED消灯（停止）。再起動しない |
| 3-10 | 通信規格をLTE-Mのみに設定（`AT+CNMP=38` / `AT+CMNB=1`） | 5秒 | リトライ |
| 3-11 | APNを設定（`AT+CGDCONT=1,"IP","soracom.io"`） | 5秒 | リトライ |
| 3-12 | 認証設定（`AT+CGAUTH=1,1,"sora","sora"`） | 5秒 | リトライ |

> SIMカードが未挿入または未認識の場合はLED消灯（`0x05`）で停止し、再起動しない（ユーザーにSIM挿入を促す）。

---

### 11.5 Phase 4: LTE-M ネットワーク接続

| ステップ | 処理 | タイムアウト | 失敗時 |
|----------|------|------------|--------|
| 4-1 | ネットワーク登録待機（`AT+CEREG?` → `0,1` または `0,5`） | 60秒 | 30秒待機後に再試行、3回失敗で再起動 |
| 4-2 | PDPコンテキスト有効化（`AT+CGACT=1,1`） | 10秒 | リトライ |
| 4-3 | アプリネットワーク設定（`AT+CNCFG=0,1,"soracom.io","sora","sora",1`） | 5秒 | リトライ |
| 4-4 | アプリネットワーク有効化（`AT+CNACT=0,1`）、`+APP PDP: 0,ACTIVE` URC 待ち | 60秒 | 5回リトライ |
| 4-5 | IPアドレス取得確認（`AT+CGPADDR=1`） | 5秒 | リトライ |
| 4-6 | 電波強度確認（`AT+CSQ`）をログ出力 | — | 無視して続行 |
| 4-7 | PSM 状態確認（`AT+CPSMS?`）をログ出力 | — | 無視して続行 |

> **AT+CNACT の成功判定**: `OK` 応答後に `+APP PDP: 0,ACTIVE` URC が到達することで成功とみなす。`OK` のみでは不十分（DEACTIVE になる場合あり）。

---

### 11.6 Phase 5: VPSサーバー接続

#### UDP接続（制御チャンネル）

| ステップ | 処理 | タイムアウト | 失敗時 |
|----------|------|------------|--------|
| 5-1 | UDP ctrlソケットオープン（VPS IP:6000） | 15秒 | バックオフ後に再試行 |
| 5-2 | `HELLO`メッセージ送信（端末ID、グループID含む） | — | — |
| 5-3 | `HELLO_ACK`受信待ち | 10秒 | 再試行 |
| 5-4 | `EVT_CONNECTED`イベントをセット | — | — |

#### UDP準備（音声チャンネル）

| ステップ | 処理 | 備考 |
|----------|------|------|
| 5-5 | UDPソケットオープン（VPS IP:6001） | — |
| 5-6 | UDPキープアライブパケット送信 | NAPTマッピング確立のため即時送信 |

---

### 11.7 Phase 6: I2S 音声初期化

| ステップ | 処理 | 失敗時 |
|----------|------|--------|
| 6-1 | I2S TX 初期化（PCM5102 DAC: I2S_NUM_0、BCK=GPIO18、WS=GPIO17、DOUT=GPIO16、16kHz ステレオ 32bit） | 再起動 |
| 6-2 | I2S RX 初期化（INMP441マイク: I2S_NUM_1、BCK=GPIO11、WS=GPIO10、DIN=GPIO9、16kHz ステレオ 32bit） | 再起動 |
| 6-3 | Opusエンコーダ初期化（16kHz、モノラル、16kbps、VOIP） | 再起動 |
| 6-4 | Opusデコーダ初期化（16kHz、モノラル） | 再起動 |

---

### 11.8 Phase 7: FreeRTOS タスク起動

全初期化完了後に各タスクを生成する。

```
Core 0:
1. ctrl_task        (Core0, 優先度5,  Stack 8KB)
2. udp_rx_task      (Core0, 優先度18, Stack 4KB)
3. udp_tx_task      (Core0, 優先度15, Stack 4KB)
4. heartbeat_task   (Core0, 優先度3,  Stack 2KB)

Core 1:
5. opus_encode_task  (Core1, 優先度20, Stack 8KB)
6. opus_decode_task  (Core1, 優先度20, Stack 8KB) ← 起動直後にデコーダ疑似スタックをウォームアップ
7. i2s_capture_task  (Core1, 優先度19, Stack 4KB) ← EVT_FLOOR_GRANTED 待ちで休止
8. i2s_playback_task (Core1, 優先度19, Stack 4KB) ← pcm_playback_queue 待ちで休止
9. ptt_task          (Core1, 優先度6,  Stack 2KB)
10. state_machine_task (Core1, 優先度4, Stack 4KB)
11. led_task          (Core1, 優先度3,  Stack 2KB)
```

全タスク起動完了後にLEDを常時点灯に変更し、待機状態へ移行する。

---

### 11.9 LED による起動状態表示

| フェーズ | LED動作 |
|----------|---------|
| Phase 1（ESP32初期化中） | 消灯（制御不可） |
| Phase 2〜7（起動シーケンス全体） | 1Hz点滅 |
| 待機状態（全接続完了） | 常時点灯 |
| SIM未検出エラー（停止） | 消灯 |
| サーバー接続失敗（リトライ中） | 1Hz点滅 |

---

### 11.10 起動時間の目安

| フェーズ | 所要時間の目安 |
|----------|--------------|
| Phase 1〜2 | 約 0.5秒 |
| Phase 3（モデム起動） | 約 3〜5秒 |
| Phase 4（LTE-M接続） | 約 10〜30秒（電波状況による） |
| Phase 5（サーバー接続） | 約 1〜3秒 |
| Phase 6〜7 | 約 0.5秒 |
| **合計** | **約 15〜40秒** |

---

## 12. エラー処理・再接続設計

### 12.1 エラー分類

| エラー種別 | 検知方法 | 影響範囲 |
|-----------|---------|---------|
| 制御チャンネル切断 | HEARTBEATタイムアウト / HELLO_ACK無応答 | 制御チャンネル全体 |
| PTT_START無応答 | ACK待ちタイムアウト（3秒） | 送話権取得失敗 |
| 送話権保持中の制御切断 | 制御チャンネル切断検知 | 送話強制終了 |
| LTE-M切断 | ATコマンドエラー / 制御チャンネル切断 | 全通信 |
| UDPパケット途絶 | ジッタバッファ枯渇 | 受話音声のみ |

---

### 12.2 制御チャンネル再接続フロー

#### 再接続バックオフ戦略

| 試行回数 | 待機時間 |
|---------|---------|
| 1回目 | 即時 |
| 2回目 | 5秒 |
| 3回目 | 10秒 |
| 4回目以降 | 30秒（上限） |

#### 再接続時の状態リセット

```
制御チャンネル切断検知
  │
  ├─ 送話権を保持していた場合
  │    → EVT_FLOOR_GRANTED をクリア（i2s_capture_task は自動停止）
  │    → サーバー側は75秒後に自動解放（HEARTBEAT タイムアウト）
  │
  ├─ EVT_CONNECTED をクリア
  ├─ EVT_DISCONNECTED をセット（LED: 1Hz点滅）
  └─ バックオフ付きで制御チャンネル再接続ループ開始

再接続成功
  │
  ├─ HELLO 送信（端末ID・グループID）
  ├─ HELLO_ACK 受信 → 新しいセッションID取得
  ├─ UDP キープアライブ送信（NAPTマッピング再確立）
  ├─ EVT_DISCONNECTED をクリア
  └─ EVT_CONNECTED をセット（LED: 常時点灯）
```

---

### 12.3 フロア制御のエッジケース

#### ケース1: PTT_START後にACKが来ない

```
PTTボタン押下
  │
  └─ PTT_START 送信 → 3秒待機
       │
       ├─ PTT_START_ACK受信 → 送話開始（正常フロー）
       │
       ├─ PTT_START_DENY受信 → 送話権取得失敗（他端末送話中）
       │    → EVT_FLOOR_BUSY をセット
       │
       └─ タイムアウト（3秒）→ 送話権取得失敗として扱う
```

#### ケース2: PTT解放後にACKが遅延到着

LTE-M の高遅延環境で起きうるケース。

```
PTTボタン押下（T=0ms）
  │ PTT_START 送信
PTTボタン解放（T=60ms）
  │ EVT_PTT_PRESSED クリア / PTT_STOP 送信
  │
  ↓ （約600ms後）
PTT_START_ACK 受信
  │
  └─ state_machine_task: EVT_PTT_PRESSED が既にクリア
       → EVT_FLOOR_GRANTED をセットしない（ACK を破棄）
       → 送信は開始されない
       ※ PTT_STOP はすでにサーバーへ送信済みのため、サーバー側は正常に処理される
```

#### ケース3: PTTボタン長押し（最大保持時間超過）

| 項目 | 値 |
|------|-----|
| 最大送話時間 | 60秒 |
| 超過時の動作 | PTT_STOP を自動送信し、送話権を解放 |

#### ケース4: UDPパケット途絶（受話中）

```
ジッタバッファが枯渇
  │
  ├─ Opusのパケットロス隠蔽（PLC）で補間（最大 25フレーム = 500ms）
  │
  └─ 500ms以上途絶 → 無音出力・待機
       （PTT_NOTIFY_STOP が来るまで受話中状態を維持）
```

---

### 12.4 LTE-M切断フロー

```
制御チャンネル接続が連続失敗
  │
  └─ ctrl_task が modem_ensure_network() を呼び出してLTE-M再接続
       → ATE0（エコー再無効化）
       → AT+CEREG? でLTE登録確認（最大30秒）
       → AT+CGACT=1,1（PDP再有効化）
       → AT+CNCFG / AT+CNACT でアプリネット再有効化
```

---

## 13. デバッグモニターアプリ設計

### 13.1 概要

VPSサーバー上のPythonプロセスがHTTPサーバーを内蔵し、デバッグ用のWebアプリを提供する。  
フロントエンドはHTML + JavaScriptのみ（フレームワークなし）。リアルタイム更新はWebSocketで行う。

```
ブラウザ（HTML+JS）
  └─ WebSocket ──► Python server :8080/   （リアルタイム: デバイス状態・ログ・音声・ループバック制御）
```

ポート8080はデバッグ用。本番運用時はファイアウォールで制限する。

---

### 13.2 機能一覧

| 機能 | 内容 |
|------|------|
| デバイスリスト | 接続中の全端末の状態をリアルタイム表示 |
| ログ | サーバーイベントをリアルタイム表示 |
| 音声モニター | 指定グループの音声をブラウザで傍受再生 |
| ループバック | PTT音声を録音して送信元へ折り返す（テスト用） |

---

### 13.3 WebSocket API

#### 接続時の初期配信

WebSocket接続確立直後にサーバーが送信する内容:

1. `snapshot`: 現在の全端末状態
2. 直近のログエントリ（最大 LOG_MAX_ENTRIES 件）
3. `audio_init`: 音声デコーダ初期化情報（sample_rate=16000, channels=1）
4. `loopback_state`: 現在のループバック有効/無効状態

#### サーバー→クライアント メッセージ一覧（JSON）

| `type` 値 | タイミング | 主なフィールド |
|-----------|-----------|--------------|
| `"snapshot"` | 接続時 | `devices` 配列 |
| `"devices"` | 端末状態変化時 | `devices` 配列 |
| `"log"` | 各種イベント時 | `level`, `device_id`, `message`, `timestamp` |
| `"audio_init"` | 接続時 | `sample_rate`=16000, `channels`=1 |
| `"audio"` | 音声中継時（サブスクライブ済みのみ） | `group`, `session`, `seq`, `ts`, `data`（base64 Opus） |
| `"loopback_state"` | 接続時・切替時 | `enabled`（true/false） |

#### クライアント→サーバー メッセージ一覧（JSON）

| `action` 値 | 内容 |
|------------|------|
| `"subscribe_audio"` | 音声フレームの配信を開始する |
| `"unsubscribe_audio"` | 音声フレームの配信を停止する |
| `"set_loopback"` | ループバック有効/無効（`enabled`: true/false） |

---

### 13.4 デバイスリスト機能

#### 表示項目

| 項目 | 内容 |
|------|------|
| 端末ID | MACアドレス下4バイト（16進数表示） |
| グループID | 現在所属グループ（1〜8） |
| 状態 | `待機中` / `送話中` / `受話中` |
| 接続時刻 | 制御チャンネル接続確立時刻 |
| 最終受信 | 最後にHEARTBEATまたはパケットを受信した時刻 |

**status 値**

| 値 | 意味 |
|----|------|
| `standby` | 待機中 |
| `transmitting` | 送話中（フロア保持） |
| `receiving` | 受話中（他端末送話中） |

---

### 13.5 音声モニター機能

#### 動作概要

1. ブラウザが `subscribe_audio` をWebSocketで送信
2. サーバーは音声UDPパケットをWebSocketにも複製送信
3. ブラウザはOpusフレーム（Base64）を受信 → デコード → Web Audio APIで再生

#### 制約・注意事項

- 音声モニターはデバッグ用途のみ。遅延・音切れは許容する
- ブラウザのAutoPlay制限のため、ユーザー操作後に再生開始する

---

### 13.6 ループバック機能

#### 動作概要

1. ブラウザで「ループバック ON」ボタンを押す
2. サーバーが音声中継の代わりにパケットをバッファに蓄積
3. PTT解放時にサーバーがバッファを送信元端末へ折り返す
4. 端末が自分の声を受信・再生する

#### サーバー側実装詳細

- セッションID `0xFF` を使用（端末の自己パケットフィルタを回避）
- 折り返し送信: PTT_NOTIFY(0xFF) → 20ms 間隔でパケット再送 → PTT_NOTIFY_STOP

---

## 14. システムアーキテクチャ

### 14.1 全体構成図

```
┌─────────────────────────────────────────────────────────────┐
│                    VPS (Debian / Python)                    │
│                                                             │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────────┐  │
│  │  ctrl_server │  │ audio_server │  │    web_server    │  │
│  │  UDP:6000    │  │  UDP:6001    │  │    port: 8080    │  │
│  └──────┬───────┘  └──────┬───────┘  └────────┬─────────┘  │
│         │                 │                    │            │
│  ┌──────▼─────────────────▼──────┐  ┌─────────▼─────────┐  │
│  │         core layer            │  │   monitor layer   │  │
│  │  device / floor / protocol    │  │  logger / relay   │  │
│  │  loopback / audio relay       │  │                   │  │
│  └───────────────────────────────┘  └───────────────────┘  │
└──────────┬───────────────┬──────────────────────────────────┘
           │ UDP:6000      │ UDP:6001
      LTE-M│          LTE-M│
┌──────────▼───┐    ┌──────▼───────┐
│  端末A        │    │  端末B        │
│ T-SIM7080G-S3│    │ T-SIM7080G-S3│
└──────────────┘    └──────────────┘
```

---

### 14.2 デバイスファームウェア アーキテクチャ

#### ディレクトリ構成

```
transceiver/
├── main/
│   ├── main.c               # app_main()・起動シーケンス
│   ├── config.h             # 全体設定定数（ポート番号・タイムアウト等）
│   └── CMakeLists.txt
├── components/
│   ├── pmic/                # AXP2101 電源管理IC
│   │   ├── axp2101.c/h      # I2Cレジスタ読み書き
│   │   └── led.c/h          # LED制御（レジスタ0x69）
│   ├── modem/               # SIM7080G モデム管理
│   │   ├── modem.c/h        # 初期化・LTE-M接続・ソケット管理
│   │   └── at_cmd.c/h       # ATコマンド送受信（UART2、Mutex排他制御）
│   ├── network/             # UDP ソケット通信
│   │   ├── ctrl_client.c/h  # 制御チャンネル（接続・送受信・再接続）
│   │   └── udp_client.c/h   # 音声チャンネル（送受信・キープアライブ）
│   ├── protocol/            # 制御プロトコル
│   │   ├── protocol.c/h     # メッセージのシリアライズ/デシリアライズ
│   │   └── floor_ctrl.c/h   # 送話権（Floor）状態管理
│   ├── audio/               # 音声入出力
│   │   ├── i2s_capture.c/h  # INMP441からPCM取得（I2S DMA、MIC_GAIN_X=8）
│   │   ├── i2s_playback.c/h # PCM5102へPCM出力（I2S DMA、SPK_GAIN_X設定）
│   │   └── jitter_buf.c/h   # 受信ジッタバッファ（深さ8、開始3フレーム）
│   ├── codec/               # Opus コーデック
│   │   └── opus_codec.c/h   # エンコード・デコード・PLC（最大25フレーム）
│   └── state/               # システム状態管理
│       └── state_machine.c/h # 状態遷移・イベントグループ・PTT/LED/HBタスク
└── managed_components/
    └── esp-libopus/         # PSRAM対応Opusライブラリ（カスタム疑似スタック）
```

#### コンポーネント依存関係

```
main
 ├── pmic        （依存なし）
 ├── modem       ← pmic
 ├── network     ← modem, protocol, state
 ├── protocol    （依存なし）
 ├── audio       ← state
 ├── codec       ← state, audio
 └── state       ← protocol
      ↑
  全タスクが state のイベントグループ・キューを参照
```

---

### 14.3 サーバー アーキテクチャ（Python）

#### ディレクトリ構成

```
server/
├── main.py          # エントリポイント。各サーバー起動・タイムアウト監視
├── config.py        # 設定読み込み（環境変数・デフォルト値）
├── protocol.py      # 制御メッセージの定義・シリアライズ
├── device.py        # 端末状態管理（登録・削除・アドレス更新）
├── floor.py         # 送話権（Floor Control）管理
├── loopback.py      # ループバック機能（バッファ・折り返し送信）
├── ctrl_server.py   # UDP制御チャンネルサーバー（port 6000）
├── audio_server.py  # UDP音声中継サーバー（port 6001）
├── web_server.py    # WebSocketサーバー（port 8080）
├── monitor.py       # WebSocketブロードキャスト管理・ログ管理
└── requirements.txt # 依存ライブラリ（websockets）
```

#### モジュール依存関係

```
main
 ├── config      （依存なし）
 ├── protocol    （依存なし）
 ├── device      ← protocol
 ├── floor       ← device
 ├── loopback    ← device, protocol
 ├── monitor     ← device
 ├── ctrl_server ← device, floor, loopback, protocol, monitor
 ├── audio_server← device, floor, loopback, protocol, monitor
 └── web_server  ← loopback, monitor
```

---

### 14.4 データフロー

#### 音声送信フロー（端末A → 端末B、同一グループ）

```
[端末A]                    [VPSサーバー]              [端末B]
  │                              │                       │
  │ PTT押下                      │                       │
  │─── UDP: PTT_START ──────────►│                       │
  │◄── UDP: PTT_START_ACK ───────│                       │
  │                              │─ UDP: PTT_NOTIFY ────►│
  │                              │                       │
  │─── UDP: Opusフレーム ────────►│                       │
  │─── UDP: Opusフレーム ────────►│──UDP: Opusフレーム ──►│
  │         （繰り返し）           │      （中継）          │
  │                              │                       │
  │ PTTボタン解放                 │                       │
  │─── UDP: PTT_STOP ───────────►│                       │
  │                              │─ UDP: PTT_NOTIFY_STOP►│
```

#### ループバックフロー（ループバックON時）

```
[端末A]                    [VPSサーバー]
  │                              │
  │─── UDP: PTT_START ──────────►│
  │◄── UDP: PTT_START_ACK ───────│
  │─── UDP: Opusフレーム ────────►│ バッファに蓄積
  │         （繰り返し）           │
  │─── UDP: PTT_STOP ───────────►│
  │                              │ 折り返し開始
  │◄── UDP: PTT_NOTIFY(0xFF) ────│
  │◄── UDP: Opusフレーム ─────────│ 20ms間隔で再送
  │◄── UDP: PTT_NOTIFY_STOP ─────│
```

---

### 14.5 設定管理

#### デバイス側（config.h 定数）

| 定数名 | 型 | 内容 | 値 |
|--------|----|------|----|
| `CONFIG_GROUP_ID` | uint8 | 所属グループID | 1 |
| `CONFIG_SERVER_IP` | string | VPSサーバーIPアドレス | 要設定 |
| `CONFIG_CTRL_PORT` | uint16 | UDP制御ポート | 6000 |
| `CONFIG_UDP_PORT` | uint16 | UDP音声ポート | 6001 |
| `CONFIG_AUDIO_SAMPLE_RATE` | uint32 | サンプリングレート | 16000 |
| `CONFIG_OPUS_BITRATE` | uint32 | Opusビットレート | 16000 |
| `CONFIG_OPUS_FRAME_MS` | uint32 | フレーム長 | 20 |
| `CONFIG_OPUS_FRAME_SAMPLES` | uint32 | フレームサンプル数 | 320 |
| `CONFIG_HEARTBEAT_INTERVAL_MS` | uint32 | ハートビート間隔 | 25000 |
| `CONFIG_PTT_DEBOUNCE_MS` | uint32 | PTTチャタリング除去時間 | 20 |

#### サーバー側（環境変数）

| 変数名 | 内容 | デフォルト |
|--------|------|-----------|
| `CTRL_PORT` | UDP制御チャンネルポート | 6000 |
| `UDP_PORT` | UDP音声チャンネルポート | 6001 |
| `WS_PORT` | WebSocketポート | 8080 |
| `HEARTBEAT_TIMEOUT` | 端末タイムアウト秒数 | 75 |
| `LOG_MAX_ENTRIES` | メモリ保持ログ最大件数 | 1000 |

---

## 15. 将来拡張

| 項目 | 内容 |
|------|------|
| ディスプレイ追加 | 小型LCD（SPI/I2C接続）によるUI強化。`display_task`をCore1に追加 |
| オーディオコーデック置換 | INMP441+PCM5102 → ES8311（ADC+DAC一体型）への移行 |
| 端末数拡張 | サーバー性能次第でスケールアップ |
| グループ数拡張 | プロトコルの変更なく対応可能（IDの拡張） |
| 暗号化 | TLS（制御チャンネル）、SRTP相当（音声チャンネル） |
| 認証 | 端末認証機構の追加 |
| スマホアプリ対応 | 同プロトコルを実装したスマホアプリとの相互接続 |

---

## 付録

### A. 用語集

| 用語 | 説明 |
|------|------|
| PTT | Push-to-Talk。ボタンを押している間だけ送話する方式 |
| 送話権（Floor） | あるグループ内で1台だけが持つことができる送信許可 |
| LTE-M | Cat-M1とも呼ばれるIoT向けLTE規格。低消費電力・低コスト |
| Opus | オープンソースの音声コーデック。低ビットレートで高音質 |
| VPS | Virtual Private Server。中継サーバーとして使用 |
| PLC | Packet Loss Concealment。パケットロス時に音声を補間するOpusの機能 |
| PSRAM | Pseudo Static RAM。ESP32-S3外付けの大容量RAM（8MB OPI） |
| NAPT | Network Address Port Translation。SORACOMでのIPアドレス共有方式 |
| DTX | Discontinued Transmission。無音区間を検出して送信を抑制する機能（本プロジェクトでは無効） |
| PSM | Power Saving Mode。モデムのスリープ機能（本プロジェクトでは無効化） |
| eDRX | Extended Discontinuous Reception。受信間隔を延ばす省電力機能（本プロジェクトでは無効化） |
