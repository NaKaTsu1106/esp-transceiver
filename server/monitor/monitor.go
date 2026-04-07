package monitor

// BroadcastDeviceUpdate はデバイス状態変化をWebSocket接続中のブラウザへ配信する
func BroadcastDeviceUpdate(sessionID uint8) {
	// TODO: Step 9で実装
}

// BroadcastAudio は音声データをモニター中のブラウザへ配信する
func BroadcastAudio(groupID uint8, deviceID uint32, seq uint16, opusFrame []byte) {
	// TODO: Step 9で実装
}
