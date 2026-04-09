package audio

import (
	"log"
	"net"

	"github.com/NaKaTsu1106/esp-transceiver/server/device"
)

// Relay は受信した音声UDPパケットを同グループの他端末へ転送する
func Relay(senderSessionID uint8, groupID uint8, pkt []byte, conn *net.UDPConn) {
	for _, d := range device.AllInGroup(groupID) {
		if d.SessionID == senderSessionID {
			continue
		}
		addr := device.GetUDPAddr(d.SessionID)
		if addr == nil {
			continue
		}
		if _, err := conn.WriteToUDP(pkt, addr); err != nil {
			log.Printf("audio relay: write to session %d failed: %v", d.SessionID, err)
		}
	}
}
