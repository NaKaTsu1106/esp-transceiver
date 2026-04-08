package server

import (
	"fmt"
	"log"
	"net/http"

	"github.com/NaKaTsu1106/esp-transceiver/server/config"
	"github.com/NaKaTsu1106/esp-transceiver/server/monitor"
)

// RunWSServer はモニターUI用HTTPサーバーを起動する（ブロッキング）。
// SSEエンドポイント: GET /events
// 静的ファイル:     GET /  → web/static/index.html
func RunWSServer(cfg *config.Config) {
	mux := http.NewServeMux()
	mux.HandleFunc("/events", monitor.ServeSSE)
	mux.Handle("/", http.FileServer(http.Dir("web/static")))

	addr := fmt.Sprintf(":%d", cfg.WSPort)
	log.Printf("Monitor server listening on %s", addr)
	if err := http.ListenAndServe(addr, mux); err != nil {
		log.Printf("Monitor server error: %v", err)
	}
}
