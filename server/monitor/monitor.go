package monitor

import (
	"encoding/json"
	"fmt"
	"net/http"
	"time"

	"github.com/NaKaTsu1106/esp-transceiver/server/device"
)

// DeviceInfo はSSE配信用の端末情報
type DeviceInfo struct {
	SessionID   uint8  `json:"session_id"`
	DeviceID    string `json:"device_id"`
	GroupID     uint8  `json:"group_id"`
	Status      string `json:"status"`
	ConnectedAt string `json:"connected_at"`
	LastSeen    string `json:"last_seen"`
}

type deviceListEvent struct {
	Type    string       `json:"type"`
	Devices []DeviceInfo `json:"devices"`
}

// deviceListJSON は現在の接続端末一覧を JSON 文字列で返す。
func deviceListJSON() string {
	devs := device.All()
	infos := make([]DeviceInfo, 0, len(devs))
	for _, d := range devs {
		infos = append(infos, DeviceInfo{
			SessionID:   d.SessionID,
			DeviceID:    fmt.Sprintf("0x%08X", d.DeviceID),
			GroupID:     d.GroupID,
			Status:      string(d.Status),
			ConnectedAt: d.ConnectedAt.Format(time.TimeOnly),
			LastSeen:    d.LastSeen.Format(time.TimeOnly),
		})
	}
	b, _ := json.Marshal(deviceListEvent{Type: "devices", Devices: infos})
	return string(b)
}

// BroadcastDeviceUpdate は端末状態変化を全 SSE クライアントへ配信する。
func BroadcastDeviceUpdate(sessionID uint8) {
	defaultHub.broadcast(deviceListJSON())
}

// BroadcastAudio は音声データをモニタリング用に配信する（Step 9で実装）。
func BroadcastAudio(groupID uint8, deviceID uint32, seq uint16, opusFrame []byte) {
	// TODO: Step 9で音声モニタリング実装
}

// ServeSSE は Server-Sent Events のHTTPハンドラー。
// 接続時に現在の端末一覧と最新ログを送信し、その後リアルタイム更新を配信する。
func ServeSSE(w http.ResponseWriter, r *http.Request) {
	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "SSE not supported", http.StatusInternalServerError)
		return
	}

	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	w.Header().Set("Access-Control-Allow-Origin", "*")

	// 先にサブスクライブしてからスナップショット送信（イベント取りこぼし防止）
	ch := defaultHub.subscribe()
	defer defaultHub.unsubscribe(ch)

	// 初期状態: 端末一覧
	fmt.Fprintf(w, "data: %s\n\n", deviceListJSON())
	// 初期状態: 最近のログ
	for _, entry := range recentLogs() {
		if b, err := json.Marshal(entry); err == nil {
			fmt.Fprintf(w, "data: %s\n\n", b)
		}
	}
	flusher.Flush()

	// リアルタイム更新ループ
	for {
		select {
		case msg, ok := <-ch:
			if !ok {
				return
			}
			fmt.Fprintf(w, "data: %s\n\n", msg)
			flusher.Flush()
		case <-r.Context().Done():
			return
		}
	}
}
