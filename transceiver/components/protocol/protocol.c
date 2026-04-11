#include "protocol.h"
#include <string.h>

int proto_encode(const ctrl_msg_t *msg, uint8_t *buf, size_t buf_len)
{
    // フォーマット: [1バイト: ペイロード長][1バイト: タイプ][ペイロード]
    size_t total = 2 + msg->payload_len;
    if (buf_len < total) return -1;
    buf[0] = msg->payload_len;
    buf[1] = msg->type;
    if (msg->payload_len > 0) {
        memcpy(&buf[2], msg->payload, msg->payload_len);
    }
    return (int)total;
}

int proto_decode(const uint8_t *buf, size_t len, ctrl_msg_t *msg)
{
    // フォーマット: [1バイト: ペイロード長][1バイト: タイプ][ペイロード]
    if (len < 2) return -1;
    uint8_t payload_len = buf[0];
    if (len < (size_t)(2 + payload_len)) return -1;
    msg->payload_len = payload_len;
    msg->type        = buf[1];
    if (payload_len > 0) {
        memcpy(msg->payload, &buf[2], payload_len);
    }
    return 2 + payload_len;
}

void proto_build_udp_header(udp_header_t *hdr, uint8_t session_id,
                              uint8_t type, uint8_t group_id,
                              uint16_t seq, uint16_t timestamp,
                              uint16_t opus_len)
{
    hdr->session_id = session_id;
    hdr->flags      = ((type & 0x01) << 7) | (group_id & 0x0F);
    hdr->seq        = seq;
    hdr->timestamp  = timestamp;
    hdr->opus_len   = opus_len;
}

uint8_t proto_udp_type(const udp_header_t *hdr)
{
    return (hdr->flags >> 7) & 0x01;
}

uint8_t proto_udp_group(const udp_header_t *hdr)
{
    return hdr->flags & 0x0F;
}
