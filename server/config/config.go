package config

import (
	"os"
	"strconv"
)

type Config struct {
	TCPPort          int
	UDPPort          int
	WSPort           int
	HeartbeatTimeout int // 秒
	LogMaxEntries    int
	LogDir           string // ログ保存ディレクトリ（空文字でファイル出力無効）
}

func Load() *Config {
	return &Config{
		TCPPort:          getEnvInt("TCP_PORT", 6000),
		UDPPort:          getEnvInt("UDP_PORT", 6001),
		WSPort:           getEnvInt("WS_PORT", 8080),
		HeartbeatTimeout: getEnvInt("HEARTBEAT_TIMEOUT", 75),
		LogMaxEntries:    getEnvInt("LOG_MAX_ENTRIES", 1000),
		LogDir:           getEnvStr("LOG_DIR", "/var/log/transceiver"),
	}
}

func getEnvInt(key string, defaultVal int) int {
	if v := os.Getenv(key); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			return n
		}
	}
	return defaultVal
}

func getEnvStr(key, defaultVal string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return defaultVal
}
