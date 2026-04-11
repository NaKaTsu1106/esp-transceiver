"""
ループバック機能: PTT 送信音声を蓄積し、PTT 終了後に送信元へ送り返す。

有効時の動作:
  - 送話権を持つ端末の音声パケットを他端末へは中継せず、セッションごとにバッファする。
  - PTT_STOP を受けたら 20ms 間隔で蓄積パケットを送信元へ送り返す。
  - 返送時は session_id を 0xFF に書き換える（デバイス側の自己パケット除去を回避）。
  - 返送前後に MSG_PTT_NOTIFY / MSG_PTT_NOTIFY_STOP を送信元へ送り、
    再生中の PTT 送信を抑制する。
"""
import asyncio
import logging

import device
import monitor
import protocol

logger = logging.getLogger("loopback")

# 返送時に使うサーバー仮 session_id（デバイス割り当て範囲 1–10 の外）
SERVER_SESSION_ID = 0xFF

# バッファ上限（50pps × 120s = 6000 パケット）
MAX_PACKETS = 6000

_enabled = False
_buffers: dict = {}  # session_id → list[bytes]  (raw UDP パケット)

_ctrl_transport  = None
_audio_transport = None


def init(ctrl, audio) -> None:
    """main.py からトランスポートを受け取る。"""
    global _ctrl_transport, _audio_transport
    _ctrl_transport  = ctrl
    _audio_transport = audio


def is_enabled() -> bool:
    return _enabled


def set_enabled(value: bool) -> None:
    global _enabled
    _enabled = value
    monitor.log("INFO", "server", f"Loopback {'enabled' if value else 'disabled'}")


def push(session_id: int, data: bytes) -> None:
    """音声パケットをバッファに追加する。上限超過時は末尾を捨てる。"""
    buf = _buffers.setdefault(session_id, [])
    if len(buf) < MAX_PACKETS:
        buf.append(data)


def clear(session_id: int) -> None:
    """セッションのバッファを破棄する（切断・タイムアウト時に呼ぶ）。"""
    _buffers.pop(session_id, None)


async def flush(session_id: int) -> None:
    """蓄積パケットを 20ms 間隔で送信元へ送り返す。"""
    packets = _buffers.pop(session_id, [])
    if not packets:
        return

    d = device.get(session_id)
    if d is None:
        return

    audio_addr = device.get_audio_addr(session_id)
    if audio_addr is None:
        logger.warning(f"Loopback: no audio addr for session={session_id}")
        return

    n = len(packets)
    logger.info(f"Loopback flush start: session={session_id} packets={n}")
    monitor.log("INFO", f"0x{d.device_id:08X}",
                f"Loopback: {n} packets → playback to session={session_id}")

    # PTT_NOTIFY(0xFF) を送信元に送り、再生中の PTT 送信を抑制する
    if _ctrl_transport and d.ctrl_addr:
        _ctrl_transport.sendto(protocol.make_ptt_notify(SERVER_SESSION_ID), d.ctrl_addr)
        await asyncio.sleep(0.050)

    for pkt in packets:
        if len(pkt) < 2:
            continue

        # byte 0 (session_id) を SERVER_SESSION_ID に差し替えて返送
        _audio_transport.sendto(bytes([SERVER_SESSION_ID]) + pkt[1:], audio_addr)

        # デバイスが切断していたら中断
        if device.get(session_id) is None:
            break

        await asyncio.sleep(0.020)

    # 再生終了を通知して PTT 送信可能に戻す
    if _ctrl_transport and d.ctrl_addr and device.get(session_id) is not None:
        _ctrl_transport.sendto(protocol.make_ptt_notify_stop(), d.ctrl_addr)

    logger.info(f"Loopback flush done: session={session_id}")
