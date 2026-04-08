package protocol

import (
	"encoding/binary"
	"errors"
	"io"
)

// TCPメッセージタイプ
const (
	MsgHello          = 0x01
	MsgHelloAck       = 0x02
	MsgPTTStart       = 0x03
	MsgPTTStartAck    = 0x04
	MsgPTTStartDeny   = 0x05
	MsgPTTStop        = 0x06
	MsgPTTNotify      = 0x07
	MsgPTTNotifyStop  = 0x08
	MsgGroupChange    = 0x09
	MsgGroupChangeAck = 0x0A
	MsgHeartbeat      = 0x0B
	MsgDisconnect     = 0x0C
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

func (h *UDPHeader) PacketType() uint8 { return (h.Flags >> 7) & 0x01 }
func (h *UDPHeader) GroupID() uint8    { return h.Flags & 0x0F }

// Encode は [payload_len][type][payload] 形式にシリアライズする
func Encode(msg *CtrlMsg) []byte {
	buf := make([]byte, 2+len(msg.Payload))
	buf[0] = uint8(len(msg.Payload))
	buf[1] = msg.Type
	copy(buf[2:], msg.Payload)
	return buf
}

// ReadMsg はTCPストリームから1メッセージを読み込む
func ReadMsg(r io.Reader) (*CtrlMsg, error) {
	hdr := make([]byte, 2)
	if _, err := io.ReadFull(r, hdr); err != nil {
		return nil, err
	}
	payLen := int(hdr[0])
	payload := make([]byte, payLen)
	if payLen > 0 {
		if _, err := io.ReadFull(r, payload); err != nil {
			return nil, err
		}
	}
	return &CtrlMsg{Type: hdr[1], Payload: payload}, nil
}

// ParseHello はHELLOペイロード [4B:device_id][1B:group_id] をパースする
func ParseHello(payload []byte) (deviceID uint32, groupID uint8, err error) {
	if len(payload) < 5 {
		return 0, 0, errors.New("HELLO payload too short")
	}
	deviceID = binary.BigEndian.Uint32(payload[0:4])
	groupID = payload[4]
	return
}

// MakeHelloAck はHELLO_ACKメッセージを生成する
func MakeHelloAck(sessionID uint8) *CtrlMsg {
	return &CtrlMsg{Type: MsgHelloAck, Payload: []byte{sessionID}}
}

// MakePTTNotify はPTT_NOTIFYメッセージを生成する
func MakePTTNotify(sessionID uint8) *CtrlMsg {
	return &CtrlMsg{Type: MsgPTTNotify, Payload: []byte{sessionID}}
}

// MakePTTNotifyStop はPTT_NOTIFY_STOPメッセージを生成する
func MakePTTNotifyStop() *CtrlMsg {
	return &CtrlMsg{Type: MsgPTTNotifyStop, Payload: nil}
}
