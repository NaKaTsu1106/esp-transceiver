package server

import (
	"encoding/binary"
	"fmt"
	"log"
	"net"

	"github.com/NaKaTsu1106/esp-transceiver/server/audio"
	"github.com/NaKaTsu1106/esp-transceiver/server/config"
	"github.com/NaKaTsu1106/esp-transceiver/server/device"
	"github.com/NaKaTsu1106/esp-transceiver/server/monitor"
	"github.com/NaKaTsu1106/esp-transceiver/server/protocol"
)

const udpMaxPktSize = 512

func RunUDPServer(cfg *config.Config) {
	addr := fmt.Sprintf(":%d", cfg.UDPPort)
	udpAddr, err := net.ResolveUDPAddr("udp", addr)
	if err != nil {
		log.Fatalf("UDP resolve addr failed: %v", err)
	}
	conn, err := net.ListenUDP("udp", udpAddr)
	if err != nil {
		log.Fatalf("UDP listen failed: %v", err)
	}
	defer conn.Close()
	log.Printf("UDP server listening on %s", addr)

	buf := make([]byte, udpMaxPktSize)
	for {
		n, remoteAddr, err := conn.ReadFromUDP(buf)
		if err != nil {
			log.Printf("UDP read error: %v", err)
			continue
		}
		if n < 6 {
			log.Printf("UDP packet too short (%d bytes) from %s", n, remoteAddr)
			continue
		}

		// UDPヘッダーパース（6バイト固定）
		// [1B: session_id][1B: flags][2B: seq][2B: timestamp]
		hdr := protocol.UDPHeader{
			SessionID: buf[0],
			Flags:     buf[1],
			Seq:       binary.BigEndian.Uint16(buf[2:4]),
			Timestamp: binary.BigEndian.Uint16(buf[4:6]),
		}
		pktType := hdr.PacketType()
		groupID := hdr.GroupID()
		sessionID := hdr.SessionID

		// セッション確認
		d := device.Get(sessionID)
		if d == nil {
			log.Printf("UDP: unknown session %d from %s", sessionID, remoteAddr)
			continue
		}

		// UDPアドレスを動的更新（NATアドレス変化に対応）
		device.UpdateUDPAddr(sessionID, remoteAddr)

		switch pktType {
		case protocol.UDPTypeKeepalive:
			monitor.Log(monitor.LevelInfo, fmt.Sprintf("0x%08X", d.DeviceID),
				"UDP keepalive session=%d addr=%s", sessionID, remoteAddr)

		case protocol.UDPTypeAudio:
			// 同グループの他端末へ中継
			pkt := make([]byte, n)
			copy(pkt, buf[:n])
			go audio.Relay(sessionID, groupID, pkt, conn)
			// モニタリング配信（リスナーがいる場合のみ）
			if n > 6 {
				monitor.BroadcastAudio(groupID, sessionID,
					hdr.Seq, hdr.Timestamp, pkt[6:])
			}

		default:
			log.Printf("UDP: unknown packet type 0x%02X from session %d", pktType, sessionID)
		}
	}
}
