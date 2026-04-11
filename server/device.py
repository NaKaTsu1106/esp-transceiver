"""
端末セッション管理。
asyncio シングルスレッドで動作するため、ロック不要。
"""
import time
from dataclasses import dataclass, field
from typing import Optional

Addr = tuple[str, int]


@dataclass
class Device:
    device_id:    int
    session_id:   int
    group_id:     int
    status:       str  = "standby"
    ctrl_addr:    Optional[Addr] = None   # 制御UDP送信元
    audio_addr:   Optional[Addr] = None   # 音声UDP送信元
    connected_at: float = field(default_factory=time.time)
    last_seen:    float = field(default_factory=time.time)


_devices:    dict[int,   Device] = {}  # session_id -> Device
_addr_index: dict[Addr,  int]    = {}  # ctrl_addr  -> session_id


# ---------- 登録 / 削除 ----------

def register(device_id: int, group_id: int, ctrl_addr: Addr) -> Optional[Device]:
    """端末を登録する。再接続の場合は既存エントリを更新して返す。満杯なら None。"""
    # 再接続: 同一 device_id を上書き
    for d in _devices.values():
        if d.device_id == device_id:
            if d.ctrl_addr and d.ctrl_addr in _addr_index:
                del _addr_index[d.ctrl_addr]
            d.group_id     = group_id
            d.ctrl_addr    = ctrl_addr
            d.status       = "standby"
            d.connected_at = time.time()
            d.last_seen    = time.time()
            _addr_index[ctrl_addr] = d.session_id
            return d

    # 新規: 空き session_id 1〜10 を探す
    for sid in range(1, 11):
        if sid not in _devices:
            d = Device(device_id=device_id, session_id=sid,
                       group_id=group_id, ctrl_addr=ctrl_addr)
            _devices[sid]          = d
            _addr_index[ctrl_addr] = sid
            return d

    return None  # 満杯


def remove(session_id: int) -> None:
    d = _devices.pop(session_id, None)
    if d and d.ctrl_addr:
        _addr_index.pop(d.ctrl_addr, None)


# ---------- 検索 ----------

def get(session_id: int) -> Optional[Device]:
    return _devices.get(session_id)


def find_by_ctrl_addr(addr: Addr) -> Optional[Device]:
    sid = _addr_index.get(addr)
    return _devices.get(sid) if sid is not None else None


def all_in_group(group_id: int) -> list[Device]:
    return [d for d in _devices.values() if d.group_id == group_id]


def all_devices() -> list[Device]:
    return list(_devices.values())


# ---------- 更新 ----------

def update_last_seen(session_id: int) -> None:
    if d := _devices.get(session_id):
        d.last_seen = time.time()


def update_ctrl_addr(session_id: int, addr: Addr) -> None:
    d = _devices.get(session_id)
    if d is None:
        return
    if d.ctrl_addr and d.ctrl_addr != addr:
        _addr_index.pop(d.ctrl_addr, None)
    d.ctrl_addr        = addr
    _addr_index[addr]  = session_id


def update_audio_addr(session_id: int, addr: Addr) -> None:
    if d := _devices.get(session_id):
        d.audio_addr = addr


def get_audio_addr(session_id: int) -> Optional[Addr]:
    if d := _devices.get(session_id):
        return d.audio_addr
    return None


def update_status(session_id: int, status: str) -> None:
    if d := _devices.get(session_id):
        d.status = status


def update_group(session_id: int, group_id: int) -> None:
    if d := _devices.get(session_id):
        d.group_id = group_id
