"""
制御メッセージ / UDPヘッダー 定義とエンコード・デコード。
デバイス側 protocol.h と対応。
"""
import struct

# 制御メッセージタイプ
MSG_HELLO            = 0x01
MSG_HELLO_ACK        = 0x02
MSG_PTT_START        = 0x03
MSG_PTT_START_ACK    = 0x04
MSG_PTT_START_DENY   = 0x05
MSG_PTT_STOP         = 0x06
MSG_PTT_NOTIFY       = 0x07
MSG_PTT_NOTIFY_STOP  = 0x08
MSG_GROUP_CHANGE     = 0x09
MSG_GROUP_CHANGE_ACK = 0x0A
MSG_HEARTBEAT        = 0x0B
MSG_DISCONNECT       = 0x0C

# 音声UDPパケットタイプ (flags bit7)
UDP_TYPE_AUDIO     = 0x00
UDP_TYPE_KEEPALIVE = 0x01


def encode(msg_type: int, payload: bytes = b"") -> bytes:
    """[1B: payload_len][1B: type][payload]"""
    return bytes([len(payload), msg_type]) + payload


def decode(data: bytes) -> tuple[int | None, bytes]:
    """Returns (msg_type, payload). msg_type=None on error."""
    if len(data) < 2:
        return None, b""
    payload_len = data[0]
    msg_type    = data[1]
    payload     = data[2 : 2 + payload_len]
    return msg_type, payload


def parse_hello(payload: bytes) -> tuple[int, int]:
    """Returns (device_id, group_id). Raises ValueError on bad payload."""
    if len(payload) < 5:
        raise ValueError("HELLO payload too short")
    device_id = struct.unpack_from("!I", payload, 0)[0]
    group_id  = payload[4]
    return device_id, group_id


def make_hello_ack(session_id: int) -> bytes:
    return encode(MSG_HELLO_ACK, bytes([session_id]))


def make_ptt_notify(session_id: int) -> bytes:
    return encode(MSG_PTT_NOTIFY, bytes([session_id]))


def make_ptt_notify_stop() -> bytes:
    return encode(MSG_PTT_NOTIFY_STOP)


def udp_packet_type(flags: int) -> int:
    return (flags >> 7) & 0x01


def udp_group_id(flags: int) -> int:
    return flags & 0x0F
