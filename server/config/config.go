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
}

func Load() *Config {
	return &Config{
		TCPPort:          getEnvInt("TCP_PORT", 6000),
		UDPPort:          getEnvInt("UDP_PORT", 6001),
		WSPort:           getEnvInt("WS_PORT", 8080),
		HeartbeatTimeout: getEnvInt("HEARTBEAT_TIMEOUT", 75),
		LogMaxEntries:    getEnvInt("LOG_MAX_ENTRIES", 1000),
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
