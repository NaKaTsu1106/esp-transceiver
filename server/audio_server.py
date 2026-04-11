"""
UDP音声サーバー（ポート6001）。
音声パケットを同グループの他端末へ中継し、モニターへも配信する。
"""
import asyncio
import logging

import device
import floor
import monitor
import protocol

logger = logging.getLogger("audio")


class AudioProtocol(asyncio.DatagramProtocol):
    def __init__(self):
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport
        logger.info(f"Audio UDP ready on {transport.get_extra_info('sockname')}")

    def datagram_received(self, data: bytes, addr: tuple):
        if len(data) < 6:
            return

        # UDPヘッダー: [1B: session_id][1B: flags][2B: seq][2B: timestamp]
        session_id = data[0]
        flags      = data[1]
        seq        = (data[2] << 8) | data[3]
        ts         = (data[4] << 8) | data[5]
        pkt_type   = protocol.udp_packet_type(flags)
        group_id   = protocol.udp_group_id(flags)

        d = device.get(session_id)
        if d is None:
            logger.debug(f"Audio: unknown session {session_id} from {addr}")
            return

        # 送信元 UDP アドレスを動的更新（NAPT 対応）
        device.update_audio_addr(session_id, addr)

        if pkt_type == protocol.UDP_TYPE_KEEPALIVE:
            monitor.log("INFO", f"0x{d.device_id:08X}",
                        f"UDP keepalive session={session_id} addr={addr[0]}:{addr[1]}")
            return

        if pkt_type == protocol.UDP_TYPE_AUDIO:
            # 送話権を持つ端末のパケットのみ中継
            if floor.holder(group_id) != session_id:
                return

            for target in device.all_in_group(group_id):
                if target.session_id == session_id:
                    continue
                audio_addr = device.get_audio_addr(target.session_id)
                if audio_addr is None:
                    logger.debug(f"Audio relay: no audio addr for session {target.session_id}")
                    continue
                self.transport.sendto(data, audio_addr)

            # WebSocket モニターへ配信（サブスクライバーがいる場合のみ）
            # ヘッダは 8 バイト（session_id, flags, seq(2), timestamp(2), opus_len(2)）
            if len(data) > 8 and monitor.has_audio_listeners():
                asyncio.ensure_future(
                    monitor.broadcast_audio(group_id, session_id, seq, ts, data[8:])
                )
