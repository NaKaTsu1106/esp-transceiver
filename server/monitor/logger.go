package monitor

import (
	"encoding/json"
	"fmt"
	"log"
	"sync"
	"time"
)

type LogLevel string

const (
	LevelInfo  LogLevel = "INFO"
	LevelWarn  LogLevel = "WARN"
	LevelError LogLevel = "ERROR"
)

// LogEntry は1件のログを表す。JSONでSSE配信される。
type LogEntry struct {
	Type     string   `json:"type"`
	Level    LogLevel `json:"level"`
	DeviceID string   `json:"device_id"`
	Message  string   `json:"message"`
	Time     string   `json:"time"`
}

const maxLogEntries = 200

var (
	logMu    sync.Mutex
	logRing  [maxLogEntries]*LogEntry
	logHead  int // 次に書き込むインデックス（0〜）
	logCount int // 保持件数（最大 maxLogEntries）
)

// Log はログを記録し、SSEで接続中のすべてのクライアントに配信する。
func Log(level LogLevel, deviceID string, format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	entry := &LogEntry{
		Type:     "log",
		Level:    level,
		DeviceID: deviceID,
		Message:  msg,
		Time:     time.Now().Format("15:04:05.000"),
	}

	log.Printf("[%s] %s %s", level, deviceID, msg)

	logMu.Lock()
	logRing[logHead%maxLogEntries] = entry
	logHead++
	if logCount < maxLogEntries {
		logCount++
	}
	logMu.Unlock()

	if b, err := json.Marshal(entry); err == nil {
		defaultHub.broadcast(string(b))
	}
}

// recentLogs は保持しているログを古い順に返す。
func recentLogs() []*LogEntry {
	logMu.Lock()
	defer logMu.Unlock()
	result := make([]*LogEntry, 0, logCount)
	start := logHead - logCount
	for i := start; i < logHead; i++ {
		if e := logRing[i%maxLogEntries]; e != nil {
			result = append(result, e)
		}
	}
	return result
}
