#pragma once
#include <stdint.h>
#include <stddef.h>

// TCPメッセージタイプ
#define MSG_HELLO           0x01
#define MSG_HELLO_ACK       0x02
#define MSG_PTT_START       0x03
#define MSG_PTT_START_ACK   0x04
#define MSG_PTT_START_DENY  0x05
#define MSG_PTT_STOP        0x06
#define MSG_PTT_NOTIFY      0x07
#define MSG_PTT_NOTIFY_STOP 0x08
#define MSG_GROUP_CHANGE    0x09
#define MSG_GROUP_CHANGE_ACK 0x0A
#define MSG_HEARTBEAT       0x0B
#define MSG_DISCONNECT      0x0C

// UDPパケットタイプ
#define UDP_TYPE_AUDIO      0x00
#define UDP_TYPE_KEEPALIVE  0x01

// 制御メッセージ構造体
typedef struct {
    uint8_t type;
    uint8_t payload[255];
    uint8_t payload_len;
} ctrl_msg_t;

// UDPパケットヘッダー（6バイト固定）
typedef struct __attribute__((packed)) {
    uint8_t  session_id;
    uint8_t  flags;         // bit7=type, bit3-0=group_id
    uint16_t seq;
    uint16_t timestamp;
} udp_header_t;

int  proto_encode(const ctrl_msg_t *msg, uint8_t *buf, size_t buf_len);
int  proto_decode(const uint8_t *buf, size_t len, ctrl_msg_t *msg);
void proto_build_udp_header(udp_header_t *hdr, uint8_t session_id,
                             uint8_t type, uint8_t group_id,
                             uint16_t seq, uint16_t timestamp);
uint8_t proto_udp_type(const udp_header_t *hdr);
uint8_t proto_udp_group(const udp_header_t *hdr);
