package device

import (
	"net"
	"sync"
	"time"
)

type Status string

const (
	StatusStandby      Status = "standby"
	StatusTransmitting Status = "transmitting"
	StatusReceiving    Status = "receiving"
	StatusDisconnected Status = "disconnected"
)

type Device struct {
	DeviceID    uint32
	SessionID   uint8
	GroupID     uint8
	Status      Status
	UDPAddr     *net.UDPAddr
	ConnectedAt time.Time
	LastSeen    time.Time
}

var (
	mu      sync.RWMutex
	devices = make(map[uint8]*Device) // key: session_id
	nextSID = uint8(1)
)

func Register(deviceID uint32, groupID uint8) *Device {
	// TODO: Step 4で実装
	return nil
}

func UpdateUDPAddr(sessionID uint8, addr *net.UDPAddr) {
	// TODO: Step 6で実装
}

func GetUDPAddr(sessionID uint8) *net.UDPAddr {
	// TODO: Step 6で実装
	return nil
}

func Remove(sessionID uint8) {
	mu.Lock()
	defer mu.Unlock()
	delete(devices, sessionID)
}

func All() []*Device {
	mu.RLock()
	defer mu.RUnlock()
	list := make([]*Device, 0, len(devices))
	for _, d := range devices {
		list = append(list, d)
	}
	return list
}
