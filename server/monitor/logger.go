package monitor

import (
	"encoding/json"
	"fmt"
	"io"
	"log"
	"os"
	"path/filepath"
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

	fileLogMu  sync.Mutex
	fileLogDir string
	fileLogF   *os.File
	fileLogDay string // 開いているファイルの日付 "2006-01-02"
)

// InitFileLogger はファイルログを有効化する。dir が空なら何もしない。
func InitFileLogger(dir string) error {
	if dir == "" {
		return nil
	}
	if err := os.MkdirAll(dir, 0755); err != nil {
		return fmt.Errorf("log dir: %w", err)
	}
	fileLogDir = dir
	return rotateIfNeeded()
}

// rotateIfNeeded は日付が変わっていれば新しいファイルに切り替える。
func rotateIfNeeded() error {
	today := time.Now().Format("2006-01-02")
	fileLogMu.Lock()
	defer fileLogMu.Unlock()
	if fileLogF != nil && fileLogDay == today {
		return nil
	}
	if fileLogF != nil {
		fileLogF.Close()
	}
	path := filepath.Join(fileLogDir, "transceiver-"+today+".log")
	f, err := os.OpenFile(path, os.O_CREATE|os.O_APPEND|os.O_WRONLY, 0644)
	if err != nil {
		return fmt.Errorf("open log file: %w", err)
	}
	fileLogF = f
	fileLogDay = today
	// 標準ログの出力先を stdout + ファイルの両方へ
	log.SetOutput(io.MultiWriter(os.Stdout, f))
	return nil
}

func writeToFile(line string) {
	if fileLogDir == "" {
		return
	}
	_ = rotateIfNeeded()
	fileLogMu.Lock()
	defer fileLogMu.Unlock()
	if fileLogF != nil {
		fmt.Fprintln(fileLogF, line)
	}
}

// Log はログを記録し、SSEで接続中のすべてのクライアントに配信する。
func Log(level LogLevel, deviceID string, format string, args ...any) {
	msg := fmt.Sprintf(format, args...)
	now := time.Now()
	entry := &LogEntry{
		Type:     "log",
		Level:    level,
		DeviceID: deviceID,
		Message:  msg,
		Time:     now.Format("15:04:05.000"),
	}

	line := fmt.Sprintf("%s [%s] %s %s", now.Format("2006-01-02 15:04:05.000"), level, deviceID, msg)
	log.Print(line)
	writeToFile(line)

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
