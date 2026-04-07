package monitor

import (
	"fmt"
	"log"
	"time"
)

type LogLevel string

const (
	LevelInfo  LogLevel = "INFO"
	LevelWarn  LogLevel = "WARN"
	LevelError LogLevel = "ERROR"
)

type LogEntry struct {
	Level     LogLevel
	Message   string
	Timestamp time.Time
	DeviceID  string
}

func Log(level LogLevel, deviceID string, format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	entry := &LogEntry{
		Level:     level,
		Message:   msg,
		Timestamp: time.Now(),
		DeviceID:  deviceID,
	}
	log.Printf("[%s] %s %s", level, deviceID, msg)
	// TODO: Step 9でWebSocket配信とメモリ保持を実装
	_ = entry
}
