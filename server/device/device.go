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
	Conn        net.Conn
	UDPAddr     *net.UDPAddr
	ConnectedAt time.Time
	LastSeen    time.Time
}

var (
	mu      sync.RWMutex
	devices = make(map[uint8]*Device) // key: session_id
)

// Register は空きセッションID（1〜10）を割り当てて端末を登録する。
// 同一deviceIDが再接続した場合は既存エントリを上書きし、古い net.Conn を返す
// （呼び出し元で Close すること）。満杯の場合は (nil, nil) を返す。
func Register(deviceID uint32, groupID uint8, conn net.Conn) (*Device, net.Conn) {
	mu.Lock()
	defer mu.Unlock()

	// 既登録の同一device_idは上書き（再接続）
	for _, d := range devices {
		if d.DeviceID == deviceID {
			oldConn := d.Conn
			d.GroupID = groupID
			d.Conn = conn
			d.Status = StatusStandby
			d.ConnectedAt = time.Now()
			d.LastSeen = time.Now()
			return d, oldConn
		}
	}

	// 空きセッションID 1〜10 を順番に探す
	for sid := uint8(1); sid <= 10; sid++ {
		if _, occupied := devices[sid]; !occupied {
			d := &Device{
				DeviceID:    deviceID,
				SessionID:   sid,
				GroupID:     groupID,
				Status:      StatusStandby,
				Conn:        conn,
				ConnectedAt: time.Now(),
				LastSeen:    time.Now(),
			}
			devices[sid] = d
			return d, nil
		}
	}
	return nil, nil // 満杯
}

// OwnedBy は指定セッションの現在の Conn が conn と同一かどうかを返す。
// 再接続で置き換えられた古いgoroutineがRemoveを誤って呼ばないためのガード。
func OwnedBy(sessionID uint8, conn net.Conn) bool {
	mu.RLock()
	defer mu.RUnlock()
	d, ok := devices[sessionID]
	return ok && d.Conn == conn
}

// UpdateLastSeen は端末の最終応答時刻を更新する
func UpdateLastSeen(sessionID uint8) {
	mu.Lock()
	defer mu.Unlock()
	if d, ok := devices[sessionID]; ok {
		d.LastSeen = time.Now()
	}
}

// Get は指定セッションIDの端末を返す
func Get(sessionID uint8) *Device {
	mu.RLock()
	defer mu.RUnlock()
	return devices[sessionID]
}

// UpdateStatus は端末のステータスを更新する
func UpdateStatus(sessionID uint8, status Status) {
	mu.Lock()
	defer mu.Unlock()
	if d, ok := devices[sessionID]; ok {
		d.Status = status
	}
}

// UpdateGroup は端末のグループIDを更新する
func UpdateGroup(sessionID uint8, groupID uint8) {
	mu.Lock()
	defer mu.Unlock()
	if d, ok := devices[sessionID]; ok {
		d.GroupID = groupID
	}
}

// AllInGroup は指定グループの接続端末を返す
func AllInGroup(groupID uint8) []*Device {
	mu.RLock()
	defer mu.RUnlock()
	list := make([]*Device, 0)
	for _, d := range devices {
		if d.GroupID == groupID {
			list = append(list, d)
		}
	}
	return list
}

func UpdateUDPAddr(sessionID uint8, addr *net.UDPAddr) {
	mu.Lock()
	defer mu.Unlock()
	if d, ok := devices[sessionID]; ok {
		d.UDPAddr = addr
	}
}

func GetUDPAddr(sessionID uint8) *net.UDPAddr {
	mu.RLock()
	defer mu.RUnlock()
	if d, ok := devices[sessionID]; ok {
		return d.UDPAddr
	}
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
