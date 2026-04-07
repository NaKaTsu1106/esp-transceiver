package main

import (
	"log"

	"github.com/NaKaTsu1106/esp-transceiver/server/config"
	srv "github.com/NaKaTsu1106/esp-transceiver/server/server"
)

func main() {
	cfg := config.Load()
	log.Printf("Starting IP Transceiver Server (TCP:%d UDP:%d WS:%d)",
		cfg.TCPPort, cfg.UDPPort, cfg.WSPort)

	// TODO: Step 4でTCPサーバー起動
	// TODO: Step 6でUDPサーバー起動
	// TODO: Step 9でWebSocketサーバー起動

	srv.RunTCPServer(cfg)
}
