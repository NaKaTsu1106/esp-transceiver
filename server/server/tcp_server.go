package server

import (
	"fmt"
	"log"
	"net"
	"time"

	"github.com/NaKaTsu1106/esp-transceiver/server/config"
	"github.com/NaKaTsu1106/esp-transceiver/server/device"
	"github.com/NaKaTsu1106/esp-transceiver/server/monitor"
	"github.com/NaKaTsu1106/esp-transceiver/server/protocol"
)

const tcpSessionTimeout = 75 * time.Second

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

	// --- HELLO ハンドシェイク ---
	conn.SetDeadline(time.Now().Add(10 * time.Second))
	msg, err := protocol.ReadMsg(conn)
	if err != nil {
		log.Printf("TCP read HELLO failed (%s): %v", conn.RemoteAddr(), err)
		return
	}
	if msg.Type != protocol.MsgHello {
		log.Printf("TCP expected HELLO(0x01), got 0x%02X from %s", msg.Type, conn.RemoteAddr())
		return
	}

	deviceID, groupID, err := protocol.ParseHello(msg.Payload)
	if err != nil {
		log.Printf("TCP HELLO parse error: %v", err)
		return
	}

	dev := device.Register(deviceID, groupID, conn)
	if dev == nil {
		log.Printf("TCP session full, rejecting device 0x%08X", deviceID)
		return
	}
	monitor.Log(monitor.LevelInfo, fmt.Sprintf("0x%08X", deviceID),
		"HELLO group=%d session=%d addr=%s", groupID, dev.SessionID, conn.RemoteAddr())
	log.Printf("TCP HELLO: device=0x%08X group=%d session=%d addr=%s",
		deviceID, groupID, dev.SessionID, conn.RemoteAddr())

	// HELLO_ACK 送信
	ack := protocol.MakeHelloAck(dev.SessionID)
	conn.SetDeadline(time.Now().Add(5 * time.Second))
	if _, err := conn.Write(protocol.Encode(ack)); err != nil {
		log.Printf("TCP send HELLO_ACK failed: %v", err)
		device.Remove(dev.SessionID)
		return
	}
	conn.SetDeadline(time.Time{}) // deadline リセット

	log.Printf("TCP session %d established (device=0x%08X)", dev.SessionID, deviceID)
	monitor.BroadcastDeviceUpdate(dev.SessionID)

	// --- セッションタイムアウト監視ゴルーチン ---
	done := make(chan struct{})
	go func() {
		ticker := time.NewTicker(10 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-done:
				return
			case <-ticker.C:
				d := device.Get(dev.SessionID)
				if d == nil {
					return
				}
				if time.Since(d.LastSeen) > tcpSessionTimeout {
					log.Printf("TCP session %d timeout (last seen %v ago), closing",
						dev.SessionID, time.Since(d.LastSeen).Round(time.Second))
					conn.Close()
					return
				}
			}
		}
	}()
	defer close(done)

	// --- メッセージ受信ループ ---
	for {
		msg, err := protocol.ReadMsg(conn)
		if err != nil {
			monitor.Log(monitor.LevelWarn, fmt.Sprintf("0x%08X", deviceID),
				"session %d disconnected: %v", dev.SessionID, err)
			log.Printf("TCP session %d disconnected: %v", dev.SessionID, err)
			break
		}

		device.UpdateLastSeen(dev.SessionID)

		switch msg.Type {
		case protocol.MsgHeartbeat:
			monitor.Log(monitor.LevelInfo, fmt.Sprintf("0x%08X", deviceID),
				"heartbeat session=%d addr=%s", dev.SessionID, conn.RemoteAddr())
			monitor.BroadcastDeviceUpdate(dev.SessionID)

		case protocol.MsgDisconnect:
			log.Printf("TCP session %d: DISCONNECT received", dev.SessionID)
			goto cleanup

		case protocol.MsgPTTStart:
			// TODO: Step 5で実装

		case protocol.MsgPTTStop:
			// TODO: Step 5で実装

		case protocol.MsgGroupChange:
			// TODO: Step 5で実装

		default:
			monitor.Log(monitor.LevelWarn, fmt.Sprintf("0x%08X", deviceID),
				"unknown message type 0x%02X", msg.Type)
			log.Printf("TCP session %d: unknown message type 0x%02X", dev.SessionID, msg.Type)
		}
	}

cleanup:
	device.Remove(dev.SessionID)
	log.Printf("TCP session %d cleaned up", dev.SessionID)
	monitor.BroadcastDeviceUpdate(dev.SessionID)
}
