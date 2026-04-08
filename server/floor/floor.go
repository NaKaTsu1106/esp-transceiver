package floor

import "sync"

var (
	mu      sync.Mutex
	holders = make(map[uint8]uint8) // group_id -> session_id (0=free)
)

// Request は送話権を要求する。成功すればtrue
func Request(groupID, sessionID uint8) bool {
	mu.Lock()
	defer mu.Unlock()
	if holders[groupID] == 0 {
		holders[groupID] = sessionID
		return true
	}
	return false
}

// Release は送話権を解放する
func Release(groupID, sessionID uint8) {
	mu.Lock()
	defer mu.Unlock()
	if holders[groupID] == sessionID {
		holders[groupID] = 0
	}
}

// Holder は現在の送話権保持者のセッションIDを返す（0=空き）
func Holder(groupID uint8) uint8 {
	mu.Lock()
	defer mu.Unlock()
	return holders[groupID]
}
