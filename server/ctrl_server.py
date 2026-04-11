"""
UDP制御サーバー（ポート6000）。
制御メッセージの受信・処理・グループへの配信を担う。
"""
import asyncio
import logging

import device
import floor
import monitor
import protocol

logger = logging.getLogger("ctrl")


class CtrlProtocol(asyncio.DatagramProtocol):
    def __init__(self):
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport
        logger.info(f"Control UDP ready on {transport.get_extra_info('sockname')}")

    def datagram_received(self, data: bytes, addr: tuple):
        asyncio.ensure_future(self._handle(data, addr))

    # ------------------------------------------------------------------ #
    # ルーティング
    # ------------------------------------------------------------------ #

    async def _handle(self, data: bytes, addr: tuple):
        msg_type, payload = protocol.decode(data)
        if msg_type is None:
            return

        if   msg_type == protocol.MSG_HELLO:        await self._on_hello(payload, addr)
        elif msg_type == protocol.MSG_PTT_START:     await self._on_ptt_start(addr)
        elif msg_type == protocol.MSG_PTT_STOP:      await self._on_ptt_stop(addr)
        elif msg_type == protocol.MSG_HEARTBEAT:     await self._on_heartbeat(addr)
        elif msg_type == protocol.MSG_DISCONNECT:    await self._on_disconnect(addr)
        elif msg_type == protocol.MSG_GROUP_CHANGE:  await self._on_group_change(payload, addr)
        else:
            logger.warning(f"Unknown ctrl msg 0x{msg_type:02X} from {addr}")

    # ------------------------------------------------------------------ #
    # ハンドラ
    # ------------------------------------------------------------------ #

    async def _on_hello(self, payload: bytes, addr: tuple):
        try:
            dev_id, grp_id = protocol.parse_hello(payload)
        except ValueError as e:
            logger.error(f"HELLO parse error from {addr}: {e}")
            return

        d = device.register(dev_id, grp_id, addr)
        if d is None:
            logger.error(f"Session full, rejecting 0x{dev_id:08X}")
            return

        monitor.log("INFO", f"0x{dev_id:08X}",
                    f"HELLO group={grp_id} session={d.session_id} addr={addr[0]}:{addr[1]}")
        self.transport.sendto(protocol.make_hello_ack(d.session_id), addr)
        await monitor.broadcast_device_update()

    async def _on_ptt_start(self, addr: tuple):
        d = device.find_by_ctrl_addr(addr)
        if d is None:
            logger.warning(f"PTT_START from unknown addr {addr}")
            return

        device.update_last_seen(d.session_id)
        device.update_ctrl_addr(d.session_id, addr)

        if floor.request(d.group_id, d.session_id):
            device.update_status(d.session_id, "transmitting")
            self.transport.sendto(protocol.encode(protocol.MSG_PTT_START_ACK), addr)
            monitor.log("INFO", f"0x{d.device_id:08X}",
                        f"PTT_START granted session={d.session_id} group={d.group_id}")
            await monitor.broadcast_device_update()
            self._send_to_group(d.group_id, d.session_id,
                                protocol.make_ptt_notify(d.session_id))
        else:
            self.transport.sendto(protocol.encode(protocol.MSG_PTT_START_DENY), addr)
            monitor.log("INFO", f"0x{d.device_id:08X}",
                        f"PTT_START denied session={d.session_id} "
                        f"holder={floor.holder(d.group_id)}")

    async def _on_ptt_stop(self, addr: tuple):
        d = device.find_by_ctrl_addr(addr)
        if d is None:
            return

        device.update_last_seen(d.session_id)
        floor.release(d.group_id, d.session_id)
        device.update_status(d.session_id, "standby")
        monitor.log("INFO", f"0x{d.device_id:08X}",
                    f"PTT_STOP session={d.session_id} group={d.group_id}")
        await monitor.broadcast_device_update()
        self._send_to_group(d.group_id, d.session_id, protocol.make_ptt_notify_stop())

    async def _on_heartbeat(self, addr: tuple):
        d = device.find_by_ctrl_addr(addr)
        if d is None:
            # アドレスが変わった可能性: HELLO 再送を促すため無視
            logger.debug(f"HEARTBEAT from unknown {addr}")
            return

        device.update_last_seen(d.session_id)
        device.update_ctrl_addr(d.session_id, addr)   # NAPT アドレス更新
        monitor.log("INFO", f"0x{d.device_id:08X}",
                    f"HEARTBEAT session={d.session_id} addr={addr[0]}:{addr[1]}")
        await monitor.broadcast_device_update()

    async def _on_disconnect(self, addr: tuple):
        d = device.find_by_ctrl_addr(addr)
        if d is None:
            return

        logger.info(f"DISCONNECT: session={d.session_id} device=0x{d.device_id:08X}")
        if floor.holder(d.group_id) == d.session_id:
            floor.release(d.group_id, d.session_id)
            self._send_to_group(d.group_id, d.session_id, protocol.make_ptt_notify_stop())
        device.remove(d.session_id)
        await monitor.broadcast_device_update()

    async def _on_group_change(self, payload: bytes, addr: tuple):
        d = device.find_by_ctrl_addr(addr)
        if d is None or len(payload) < 1:
            return

        device.update_last_seen(d.session_id)
        new_group = payload[0]
        old_group = d.group_id

        if floor.holder(d.group_id) == d.session_id:
            floor.release(d.group_id, d.session_id)
        device.update_status(d.session_id, "standby")
        device.update_group(d.session_id, new_group)
        d.group_id = new_group

        self.transport.sendto(
            protocol.encode(protocol.MSG_GROUP_CHANGE_ACK, bytes([new_group])), addr)
        monitor.log("INFO", f"0x{d.device_id:08X}",
                    f"GROUP_CHANGE session={d.session_id} {old_group}→{new_group}")
        await monitor.broadcast_device_update()

    # ------------------------------------------------------------------ #
    # ヘルパー
    # ------------------------------------------------------------------ #

    def _send_to_group(self, group_id: int, sender_sid: int, data: bytes):
        """グループ内の sender 以外の全端末へ制御パケットを送信する。"""
        for d in device.all_in_group(group_id):
            if d.session_id == sender_sid or d.ctrl_addr is None:
                continue
            self.transport.sendto(data, d.ctrl_addr)
