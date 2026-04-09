package monitor

import (
	"encoding/base64"
	"encoding/json"
	"fmt"
	"net/http"
	"sync"
)

type audioFrame struct {
	Type      string `json:"type"`
	Group     uint8  `json:"group"`
	Session   uint8  `json:"session"`
	Seq       uint16 `json:"seq"`
	Timestamp uint16 `json:"ts"`
	Data      string `json:"data"` // base64 エンコード Opus フレーム
}

type audioHub struct {
	mu      sync.Mutex
	clients map[chan string]struct{}
}

var defaultAudioHub = &audioHub{
	clients: make(map[chan string]struct{}),
}

func (h *audioHub) subscribe() chan string {
	ch := make(chan string, 128) // 音声は頻度が高いため深めのバッファ
	h.mu.Lock()
	h.clients[ch] = struct{}{}
	h.mu.Unlock()
	return ch
}

func (h *audioHub) unsubscribe(ch chan string) {
	h.mu.Lock()
	delete(h.clients, ch)
	h.mu.Unlock()
	close(ch)
}

func (h *audioHub) broadcast(msg string) {
	h.mu.Lock()
	defer h.mu.Unlock()
	for ch := range h.clients {
		select {
		case ch <- msg:
		default:
			// 遅延クライアントはスキップ（非ブロッキング）
		}
	}
}

// HasListeners は音声モニタリング中のクライアントがいるかを返す。
// クライアントがいない場合は BroadcastAudio のエンコードを省略できる。
func HasAudioListeners() bool {
	defaultAudioHub.mu.Lock()
	defer defaultAudioHub.mu.Unlock()
	return len(defaultAudioHub.clients) > 0
}

// ServeAudioSSE は音声フレームをSSEで配信するHTTPハンドラー。
func ServeAudioSSE(w http.ResponseWriter, r *http.Request) {
	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "SSE not supported", http.StatusInternalServerError)
		return
	}

	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	w.Header().Set("Connection", "keep-alive")
	w.Header().Set("Access-Control-Allow-Origin", "*")

	ch := defaultAudioHub.subscribe()
	defer defaultAudioHub.unsubscribe(ch)

	// 初期パケット: クライアント側でデコーダ初期化に使う設定情報
	init := map[string]any{"type": "audio_init", "sample_rate": 8000, "channels": 1}
	if b, err := json.Marshal(init); err == nil {
		fmt.Fprintf(w, "data: %s\n\n", b)
		flusher.Flush()
	}

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

// broadcastAudioFrame は内部から呼ばれる実装。
func broadcastAudioFrame(groupID, sessionID uint8, seq, timestamp uint16, opus []byte) {
	if !HasAudioListeners() {
		return
	}
	frame := audioFrame{
		Type:      "audio",
		Group:     groupID,
		Session:   sessionID,
		Seq:       seq,
		Timestamp: timestamp,
		Data:      base64.StdEncoding.EncodeToString(opus),
	}
	b, err := json.Marshal(frame)
	if err != nil {
		return
	}
	defaultAudioHub.broadcast(string(b))
}
