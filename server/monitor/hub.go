package monitor

import "sync"

// hub は SSE クライアントへのブロードキャストを管理する
type hub struct {
	mu      sync.Mutex
	clients map[chan string]struct{}
}

var defaultHub = &hub{clients: make(map[chan string]struct{})}

func (h *hub) subscribe() chan string {
	ch := make(chan string, 64)
	h.mu.Lock()
	h.clients[ch] = struct{}{}
	h.mu.Unlock()
	return ch
}

func (h *hub) unsubscribe(ch chan string) {
	h.mu.Lock()
	delete(h.clients, ch)
	h.mu.Unlock()
	close(ch)
}

func (h *hub) broadcast(msg string) {
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
