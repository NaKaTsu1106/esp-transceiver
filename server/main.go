package main

import (
	"log"

	"github.com/NaKaTsu1106/esp-transceiver/server/config"
	"github.com/NaKaTsu1106/esp-transceiver/server/monitor"
	srv "github.com/NaKaTsu1106/esp-transceiver/server/server"
)

func main() {
	cfg := config.Load()

	if err := monitor.InitFileLogger(cfg.LogDir); err != nil {
		log.Printf("WARNING: file logging disabled: %v", err)
	}

	log.Printf("Starting IP Transceiver Server (TCP:%d UDP:%d Monitor:%d)",
		cfg.TCPPort, cfg.UDPPort, cfg.WSPort)

	go srv.RunWSServer(cfg)

	// TODO: Step 6でUDPサーバー起動
	// go srv.RunUDPServer(cfg)

	srv.RunTCPServer(cfg)
}
