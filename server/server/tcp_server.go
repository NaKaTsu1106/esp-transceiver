package server

import (
	"fmt"
	"log"
	"net"

	"github.com/NaKaTsu1106/esp-transceiver/server/config"
)

func RunTCPServer(cfg *config.Config) {
	addr := fmt.Sprintf(":%d", cfg.TCPPort)
	ln, err := net.Listen("tcp", addr)
	if err != nil {
		log.Fatalf("TCP listen failed: %v", err)
	}
	log.Printf("TCP server listening on %s", addr)

	for {
		conn, err := ln.Accept()
		if err != nil {
			log.Printf("TCP accept error: %v", err)
			continue
		}
		go handleTCPConn(conn)
	}
}

func handleTCPConn(conn net.Conn) {
	defer conn.Close()
	log.Printf("TCP connected: %s", conn.RemoteAddr())
	// TODO: Step 4でHELLO/HEARTBEAT/PTT処理を実装
}
