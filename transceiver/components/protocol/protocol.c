#include "protocol.h"
#include <string.h>

int proto_encode(const ctrl_msg_t *msg, uint8_t *buf, size_t buf_len)
{
    // TODO: Step 4で実装
    // フォーマット: [1バイト: ペイロード長][1バイト: タイプ][ペイロード]
    return 0;
}

int proto_decode(const uint8_t *buf, size_t len, ctrl_msg_t *msg)
{
    // TODO: Step 4で実装
    return 0;
}

void proto_build_udp_header(udp_header_t *hdr, uint8_t session_id,
                              uint8_t type, uint8_t group_id,
                              uint16_t seq, uint16_t timestamp)
{
    hdr->session_id = session_id;
    hdr->flags      = ((type & 0x01) << 7) | (group_id & 0x0F);
    hdr->seq        = seq;
    hdr->timestamp  = timestamp;
}

uint8_t proto_udp_type(const udp_header_t *hdr)
{
    return (hdr->flags >> 7) & 0x01;
}

uint8_t proto_udp_group(const udp_header_t *hdr)
{
    return hdr->flags & 0x0F;
}
