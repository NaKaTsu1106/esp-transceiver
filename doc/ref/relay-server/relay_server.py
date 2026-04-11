#!/usr/bin/env python3
"""
IP Radio PTT Relay Server with WebSocket Debug Monitor
=======================================================
RTP + Opus over LTE Cat-M1 — PTT Group Communication System

Features:
  - Control channel (UDP 5000): JOIN/LEAVE/PTT_ON/PTT_OFF/HEARTBEAT/ACK/PTT_DENY
  - Audio channel  (UDP 5002): RTP packet relay (passthrough, no decode)
  - WebSocket monitor (WS 8765): Real-time packet & state viewer for browser

Usage:
  pip install websockets
  python relay_server.py [--control-port 5000] [--audio-port 5002] [--ws-port 8765]
"""

import asyncio
import struct
import json
import time
import logging
import argparse
import base64
from typing import Optional
from collections import deque

logger = logging.getLogger("relay")

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
# Control packet types
TYPE_JOIN      = 0x01
TYPE_LEAVE     = 0x02
TYPE_PTT_ON    = 0x03
TYPE_PTT_OFF   = 0x04
TYPE_HEARTBEAT = 0x05
TYPE_ACK       = 0x06
TYPE_PTT_DENY  = 0x07

TYPE_NAMES = {
    0x01: "JOIN", 0x02: "LEAVE", 0x03: "PTT_ON", 0x04: "PTT_OFF",
    0x05: "HEARTBEAT", 0x06: "ACK", 0x07: "PTT_DENY",
}

# ACK status codes
ACK_OK           = 0x00
ACK_GROUP_FULL   = 0x01
ACK_INVALID_GID  = 0x02
ACK_NOT_MEMBER   = 0x03

HEARTBEAT_TIMEOUT = 60  # seconds
MAX_GROUP_MEMBERS = 256

# Control packet header: type(1) + group_id(1) + ssrc(4) + reserved(2) = 8 bytes
CTRL_HEADER = struct.Struct("!BBIxx")
CTRL_HEADER_WITH_RESERVED = struct.Struct("!BBIH")  # for building with explicit reserved

# ---------------------------------------------------------------------------
# Group State
# ---------------------------------------------------------------------------
class GroupState:
    def __init__(self, group_id: int, audio_port: int):
        self.group_id = group_id
        self.audio_port = audio_port
        self.floor_holder: Optional[int] = None  # SSRC of current speaker
        self.members: dict[int, dict] = {}  # ssrc -> member info

    def to_dict(self) -> dict:
        return {
            "group_id": self.group_id,
            "audio_port": self.audio_port,
            "floor_holder": self.floor_holder,
            "members": {
                str(ssrc): {
                    "ssrc": f"0x{ssrc:08X}",
                    "ctrl_addr": f"{m['ctrl_addr'][0]}:{m['ctrl_addr'][1]}",
                    "rtp_addr": f"{m['rtp_addr'][0]}:{m['rtp_addr'][1]}",
                    "last_seen": round(time.time() - m["last_seen"], 1),
                }
                for ssrc, m in self.members.items()
            },
        }


# ---------------------------------------------------------------------------
# Monitor Hub — broadcasts events to WebSocket clients
# ---------------------------------------------------------------------------
class MonitorHub:
    def __init__(self, maxlen: int = 500):
        self.clients: set = set()
        self.audio_clients: set = set()   # audio stream subscribers
        self.event_log: deque = deque(maxlen=maxlen)

    async def register(self, ws):
        self.clients.add(ws)
        logger.info(f"Monitor client connected ({len(self.clients)} total)")

    def unregister(self, ws):
        self.clients.discard(ws)
        self.audio_clients.discard(ws)
        logger.info(f"Monitor client disconnected ({len(self.clients)} total)")

    async def broadcast(self, event: dict):
        event["_ts"] = time.time()
        event["_iso"] = time.strftime("%H:%M:%S", time.localtime(event["_ts"]))
        self.event_log.append(event)
        if not self.clients:
            return
        msg = json.dumps(event)
        dead = set()
        for ws in self.clients:
            try:
                await ws.send(msg)
            except Exception:
                dead.add(ws)
        for ws in dead:
            self.unregister(ws)

    async def broadcast_audio(self, event: dict):
        """Opus ペイロードを音声サブスクライバーにのみ送信 (ログ・タイムスタンプなし)"""
        if not self.audio_clients:
            return
        msg = json.dumps(event)
        dead = set()
        for ws in self.audio_clients:
            try:
                await ws.send(msg)
            except Exception:
                dead.add(ws)
        for ws in dead:
            self.audio_clients.discard(ws)


# ---------------------------------------------------------------------------
# Control Channel Protocol (UDP 5000)
# ---------------------------------------------------------------------------
class ControlProtocol(asyncio.DatagramProtocol):
    def __init__(self, groups: dict[int, GroupState], monitor: MonitorHub):
        self.groups = groups
        self.monitor = monitor
        self.transport = None

    def connection_made(self, transport):
        self.transport = transport
        logger.info(f"Control channel ready on {transport.get_extra_info('sockname')}")

    def datagram_received(self, data: bytes, addr: tuple):
        if len(data) < 8:
            return
        ptype, group_id, ssrc = struct.unpack_from("!BBI", data, 0)
        payload = data[8:]
        asyncio.ensure_future(self._handle(ptype, group_id, ssrc, payload, addr))

    async def _handle(self, ptype: int, group_id: int, ssrc: int, payload: bytes, addr: tuple):
        name = TYPE_NAMES.get(ptype, f"UNKNOWN(0x{ptype:02X})")
        ev_base = {
            "channel": "control",
            "type": name,
            "group_id": group_id,
            "ssrc": f"0x{ssrc:08X}",
            "src": f"{addr[0]}:{addr[1]}",
        }

        group = self.groups.get(group_id)

        if ptype == TYPE_JOIN:
            if group is None:
                # Auto-create group with default audio port
                audio_port = 5002 + (group_id * 2)
                group = GroupState(group_id, audio_port)
                self.groups[group_id] = group
                logger.info(f"Auto-created group {group_id} (audio port {audio_port})")

            if len(group.members) >= MAX_GROUP_MEMBERS:
                self._send_ack(addr, group_id, ssrc, ACK_GROUP_FULL)
                ev_base["result"] = "GROUP_FULL"
                await self.monitor.broadcast(ev_base)
                return

            audio_port_client = struct.unpack_from("!H", payload, 0)[0] if len(payload) >= 2 else group.audio_port
            group.members[ssrc] = {
                "ctrl_addr": addr,
                "rtp_addr": (addr[0], audio_port_client),
                "last_seen": time.time(),
            }
            self._send_ack(addr, group_id, ssrc, ACK_OK)
            ev_base["result"] = "OK"
            ev_base["member_count"] = len(group.members)
            logger.info(f"JOIN group={group_id} ssrc=0x{ssrc:08X} from {addr} members={len(group.members)}")

        elif ptype == TYPE_LEAVE:
            if group and ssrc in group.members:
                del group.members[ssrc]
                if group.floor_holder == ssrc:
                    group.floor_holder = None
                logger.info(f"LEAVE group={group_id} ssrc=0x{ssrc:08X}")
            ev_base["member_count"] = len(group.members) if group else 0

        elif ptype == TYPE_PTT_ON:
            if group is None or ssrc not in group.members:
                self._send_ack(addr, group_id, ssrc, ACK_NOT_MEMBER)
                ev_base["result"] = "NOT_MEMBER"
                await self.monitor.broadcast(ev_base)
                return

            if group.floor_holder is None:
                group.floor_holder = ssrc
                self._send_ack(addr, group_id, ssrc, ACK_OK)
                self._broadcast_ctrl(group, ptype, group_id, ssrc, exclude=None)
                ev_base["result"] = "GRANTED"
                logger.info(f"PTT_ON granted group={group_id} ssrc=0x{ssrc:08X}")
            elif group.floor_holder == ssrc:
                # 同一SSRCからの再送: ACK_OK を返すだけ (broadcast は不要)
                self._send_ack(addr, group_id, ssrc, ACK_OK)
                ev_base["result"] = "RE-GRANTED"
                logger.info(f"PTT_ON re-granted (same holder) group={group_id} ssrc=0x{ssrc:08X}")
            else:
                self._send_ptt_deny(addr, group_id, ssrc, group.floor_holder)
                ev_base["result"] = "DENIED"
                ev_base["holder"] = f"0x{group.floor_holder:08X}"
                logger.info(f"PTT_ON denied group={group_id} ssrc=0x{ssrc:08X} holder=0x{group.floor_holder:08X}")

        elif ptype == TYPE_PTT_OFF:
            if group and group.floor_holder == ssrc:
                group.floor_holder = None
                self._broadcast_ctrl(group, ptype, group_id, ssrc, exclude=None)
                ev_base["result"] = "RELEASED"
                logger.info(f"PTT_OFF group={group_id} ssrc=0x{ssrc:08X}")

        elif ptype == TYPE_HEARTBEAT:
            if group and ssrc in group.members:
                group.members[ssrc]["last_seen"] = time.time()
                self._send_ack(addr, group_id, ssrc, ACK_OK)
            ev_base["result"] = "OK"

        else:
            ev_base["result"] = "UNKNOWN_TYPE"

        # Attach current group snapshot
        if group:
            ev_base["group_state"] = group.to_dict()
        await self.monitor.broadcast(ev_base)

    def _send_ack(self, addr, group_id, ssrc, status):
        pkt = struct.pack("!BBIH", TYPE_ACK, group_id, ssrc, 0) + struct.pack("!B", status)
        self.transport.sendto(pkt, addr)

    def _send_ptt_deny(self, addr, group_id, ssrc, active_ssrc):
        pkt = struct.pack("!BBIH", TYPE_PTT_DENY, group_id, ssrc, 0) + struct.pack("!I", active_ssrc)
        self.transport.sendto(pkt, addr)

    def _broadcast_ctrl(self, group: GroupState, ptype, group_id, ssrc, exclude=None):
        pkt = struct.pack("!BBIH", ptype, group_id, ssrc, 0)
        for member_ssrc, member in group.members.items():
            if member_ssrc != exclude:
                self.transport.sendto(pkt, member["ctrl_addr"])


# ---------------------------------------------------------------------------
# Audio Channel Protocol (UDP 5002+) — RTP passthrough
# ---------------------------------------------------------------------------
class AudioProtocol(asyncio.DatagramProtocol):
    def __init__(self, group: GroupState, monitor: MonitorHub):
        self.group = group
        self.monitor = monitor
        self.transport = None
        self._pkt_count = 0

    def connection_made(self, transport):
        self.transport = transport
        logger.info(f"Audio channel ready on {transport.get_extra_info('sockname')} (group {self.group.group_id})")

    def datagram_received(self, data: bytes, addr: tuple):
        if len(data) < 12:
            return

        # Parse RTP header (first 12 bytes)
        byte0, byte1, seq, ts, ssrc = struct.unpack_from("!BBHII", data, 0)
        version = (byte0 >> 6) & 0x03
        marker = (byte1 >> 7) & 0x01
        pt = byte1 & 0x7F

        if version != 2:
            return

        # Only relay if this SSRC holds the floor
        if self.group.floor_holder != ssrc:
            return

        # Relay to all other members
        relayed_to = 0
        for member_ssrc, member in self.group.members.items():
            if member_ssrc != ssrc:
                self.transport.sendto(data, member["rtp_addr"])
                relayed_to += 1

        # 音声モニター: サブスクライバーがいれば全パケットの Opus ペイロードを送信
        if self.monitor.audio_clients:
            asyncio.ensure_future(self.monitor.broadcast_audio({
                "channel": "audio_stream",
                "ssrc": f"0x{ssrc:08X}",
                "seq": seq,
                "ts": ts,
                "opus_b64": base64.b64encode(data[12:]).decode(),
            }))

        # Broadcast monitor event (throttled: every 10th packet to avoid flood)
        self._pkt_count += 1
        if self._pkt_count % 10 == 1 or marker == 1:
            ev = {
                "channel": "audio",
                "type": "RTP",
                "group_id": self.group.group_id,
                "ssrc": f"0x{ssrc:08X}",
                "seq": seq,
                "timestamp": ts,
                "marker": marker,
                "pt": pt,
                "payload_size": len(data) - 12,
                "total_size": len(data),
                "relayed_to": relayed_to,
                "src": f"{addr[0]}:{addr[1]}",
            }
            asyncio.ensure_future(self.monitor.broadcast(ev))


# ---------------------------------------------------------------------------
# Timeout Reaper
# ---------------------------------------------------------------------------
async def reaper_task(groups: dict[int, GroupState], monitor: MonitorHub, interval: float = 10):
    while True:
        await asyncio.sleep(interval)
        now = time.time()
        for gid, group in list(groups.items()):
            stale = [
                ssrc for ssrc, m in group.members.items()
                if now - m["last_seen"] > HEARTBEAT_TIMEOUT
            ]
            for ssrc in stale:
                del group.members[ssrc]
                if group.floor_holder == ssrc:
                    group.floor_holder = None
                logger.info(f"Timeout: removed ssrc=0x{ssrc:08X} from group {gid}")
                await monitor.broadcast({
                    "channel": "control",
                    "type": "TIMEOUT",
                    "group_id": gid,
                    "ssrc": f"0x{ssrc:08X}",
                    "group_state": group.to_dict(),
                })


# ---------------------------------------------------------------------------
# WebSocket Server
# ---------------------------------------------------------------------------
async def ws_handler(ws, monitor: MonitorHub, groups: dict[int, GroupState]):
    await monitor.register(ws)

    # Send current state snapshot
    snapshot = {
        "channel": "system",
        "type": "SNAPSHOT",
        "groups": {str(gid): g.to_dict() for gid, g in groups.items()},
    }
    snapshot["_ts"] = time.time()
    snapshot["_iso"] = time.strftime("%H:%M:%S", time.localtime(snapshot["_ts"]))
    await ws.send(json.dumps(snapshot))

    # Send recent event log
    for ev in monitor.event_log:
        await ws.send(json.dumps(ev))

    try:
        async for msg in ws:
            try:
                data = json.loads(msg)
                action = data.get("action")
                if action == "subscribe_audio":
                    monitor.audio_clients.add(ws)
                    logger.info(f"Audio subscribe ({len(monitor.audio_clients)} listeners)")
                elif action == "unsubscribe_audio":
                    monitor.audio_clients.discard(ws)
                    logger.info(f"Audio unsubscribe ({len(monitor.audio_clients)} listeners)")
            except Exception:
                pass
    except Exception:
        pass
    finally:
        monitor.unregister(ws)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
async def main(control_port: int, audio_port: int, ws_port: int):
    logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(name)s] %(message)s")

    monitor = MonitorHub()
    groups: dict[int, GroupState] = {}

    # Pre-create default group 0
    groups[0] = GroupState(group_id=0, audio_port=audio_port)

    loop = asyncio.get_running_loop()

    # Control channel
    ctrl_transport, _ = await loop.create_datagram_endpoint(
        lambda: ControlProtocol(groups, monitor),
        local_addr=("0.0.0.0", control_port),
    )

    # Audio channel for group 0
    audio_transport, _ = await loop.create_datagram_endpoint(
        lambda: AudioProtocol(groups[0], monitor),
        local_addr=("0.0.0.0", audio_port),
    )

    # WebSocket server
    try:
        import websockets
    except ImportError:
        logger.error("websockets not installed. Run: pip install websockets")
        return

    ws_server = await websockets.serve(
        lambda ws: ws_handler(ws, monitor, groups),
        "0.0.0.0",
        ws_port,
    )
    logger.info(f"WebSocket monitor on ws://0.0.0.0:{ws_port}")

    # Start reaper
    asyncio.create_task(reaper_task(groups, monitor))

    logger.info("Relay server running. Press Ctrl+C to stop.")
    await monitor.broadcast({
        "channel": "system",
        "type": "SERVER_START",
        "control_port": control_port,
        "audio_port": audio_port,
        "ws_port": ws_port,
    })

    try:
        await asyncio.Future()  # run forever
    except asyncio.CancelledError:
        pass
    finally:
        ctrl_transport.close()
        audio_transport.close()
        ws_server.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="PTT Relay Server")
    parser.add_argument("--control-port", type=int, default=5000)
    parser.add_argument("--audio-port", type=int, default=5002)
    parser.add_argument("--ws-port", type=int, default=8765)
    args = parser.parse_args()

    asyncio.run(main(args.control_port, args.audio_port, args.ws_port))
