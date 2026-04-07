package protocol

// TCPメッセージタイプ
const (
	MsgHello         = 0x01
	MsgHelloAck      = 0x02
	MsgPTTStart      = 0x03
	MsgPTTStartAck   = 0x04
	MsgPTTStartDeny  = 0x05
	MsgPTTStop       = 0x06
	MsgPTTNotify     = 0x07
	MsgPTTNotifyStop = 0x08
	MsgGroupChange   = 0x09
	MsgGroupChangeAck = 0x0A
	MsgHeartbeat     = 0x0B
	MsgDisconnect    = 0x0C
)

// UDPパケットタイプ
const (
	UDPTypeAudio     = 0x00
	UDPTypeKeepalive = 0x01
)

// テストトーン予約セッションID
const SessionIDTestTone = 0xFF

// CtrlMsg はTCP制御メッセージを表す
type CtrlMsg struct {
	Type    uint8
	Payload []byte
}

// UDPHeader はUDPパケットヘッダー（6バイト固定）
type UDPHeader struct {
	SessionID uint8
	Flags     uint8  // bit7=type, bit3-0=group_id
	Seq       uint16
	Timestamp uint16
}

func (h *UDPHeader) PacketType() uint8 {
	return (h.Flags >> 7) & 0x01
}

func (h *UDPHeader) GroupID() uint8 {
	return h.Flags & 0x0F
}

// TODO: Step 4でエンコード/デコード実装
func Encode(msg *CtrlMsg) []byte { return nil }
func Decode(buf []byte) (*CtrlMsg, error) { return nil, nil }
