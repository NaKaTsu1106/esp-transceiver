"""
WebSocket ブロードキャスト管理・ログ管理。
websockets ライブラリの ServerConnection を対象とする。
"""
import asyncio
import base64
import json
import logging
import os
import time
from collections import deque
from typing import TYPE_CHECKING

logger = logging.getLogger("monitor")

# WebSocket クライアント管理（asyncio シングルスレッドのためロック不要）
_clients:       set = set()   # 全接続 WebSocket (websockets ServerConnection)
_audio_clients: set = set()   # 音声サブスクライバー

# ログリングバッファ（最大 200 件）
_log_ring: deque = deque(maxlen=200)

# ファイルログ
_log_dir: str = ""
_log_file      = None
_log_day: str  = ""


# ---------- ファイルログ ----------

def init_file_logger(log_dir: str) -> None:
    global _log_dir
    if not log_dir:
        return
    os.makedirs(log_dir, exist_ok=True)
    _log_dir = log_dir
    _rotate_if_needed()


def _rotate_if_needed() -> None:
    global _log_file, _log_day
    today = time.strftime("%Y-%m-%d")
    if _log_file and _log_day == today:
        return
    if _log_file:
        _log_file.close()
    path   = os.path.join(_log_dir, f"transceiver-{today}.log")
    _log_file = open(path, "a", encoding="utf-8")
    _log_day  = today


# ---------- ログ記録 ----------

def log(level: str, device_id: str, message: str) -> None:
    """ログをバッファ・ファイル・WebSocket に配信する。"""
    now    = time.time()
    ms     = int((now % 1) * 1000)
    ts_str = time.strftime("%H:%M:%S", time.localtime(now)) + f".{ms:03d}"
    entry  = {
        "type":      "log",
        "level":     level,
        "device_id": device_id,
        "message":   message,
        "time":      ts_str,
    }
    line = (f"{time.strftime('%Y-%m-%d %H:%M:%S', time.localtime(now))}"
            f" [{level}] {device_id} {message}")
    logger.info(line)

    if _log_dir:
        _rotate_if_needed()
        if _log_file:
            _log_file.write(line + "\n")
            _log_file.flush()

    _log_ring.append(entry)
    asyncio.ensure_future(_broadcast(entry))


def recent_logs() -> list:
    return list(_log_ring)


# ---------- クライアント管理 ----------

def add_client(ws) -> None:
    _clients.add(ws)
    logger.info(f"WS client connected ({len(_clients)} total)")


def remove_client(ws) -> None:
    _clients.discard(ws)
    _audio_clients.discard(ws)
    logger.info(f"WS client disconnected ({len(_clients)} total)")


def subscribe_audio(ws) -> None:
    _audio_clients.add(ws)
    logger.info(f"Audio subscribe ({len(_audio_clients)} listeners)")


def unsubscribe_audio(ws) -> None:
    _audio_clients.discard(ws)
    logger.info(f"Audio unsubscribe ({len(_audio_clients)} listeners)")


def has_audio_listeners() -> bool:
    return bool(_audio_clients)


# ---------- ブロードキャスト ----------

async def _broadcast(event: dict) -> None:
    if not _clients:
        return
    msg  = json.dumps(event)
    dead = set()
    for ws in list(_clients):
        try:
            await ws.send(msg)
        except Exception:
            dead.add(ws)
    for ws in dead:
        remove_client(ws)


async def broadcast_device_update() -> None:
    import device as dev
    import time as _t
    devices_info = [
        {
            "session_id":   d.session_id,
            "device_id":    f"0x{d.device_id:08X}",
            "group_id":     d.group_id,
            "status":       d.status,
            "connected_at": _t.strftime("%H:%M:%S", _t.localtime(d.connected_at)),
            "last_seen":    _t.strftime("%H:%M:%S", _t.localtime(d.last_seen)),
        }
        for d in dev.all_devices()
    ]
    await _broadcast({"type": "devices", "devices": devices_info})


async def broadcast_audio(group_id: int, session_id: int,
                          seq: int, ts: int, opus: bytes) -> None:
    if not _audio_clients:
        return
    event = {
        "type":    "audio",
        "group":   group_id,
        "session": session_id,
        "seq":     seq,
        "ts":      ts,
        "data":    base64.b64encode(opus).decode(),
    }
    msg  = json.dumps(event)
    dead = set()
    for ws in list(_audio_clients):
        try:
            await ws.send(msg)
        except Exception:
            dead.add(ws)
    for ws in dead:
        remove_client(ws)
