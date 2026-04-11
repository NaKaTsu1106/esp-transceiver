"""
1kHz テストトーンを Opus エンコードして端末に直接送信する。
デバッグ用: モニターの Tone ボタンから呼び出す。
"""
import asyncio
import math
import struct
import logging

import opuslib

import ctrl_server
import device
import monitor
import protocol

logger = logging.getLogger("tone")

SAMPLE_RATE   = 16000
CHANNELS      = 1
FRAME_SAMPLES = 320    # 20ms @ 16kHz
BITRATE       = 16000
TONE_FREQ_HZ  = 1000
# 端末側 SPK_GAIN_X=8 でクリップしない最大振幅 = 32767/8 = 4096。
# 余裕を持って 2048 (-24 dBFS) に設定する。
# 旧値 16000 は SPK_GAIN_X=32 時代の設定で、現在は矩形波になってしまう。
TONE_AMP      = 2048

# サーバー発の仮想セッションID。
# 端末は自分自身の session_id と一致するパケットを捨てるため、
# 割り当て範囲外の 0xFF を使う（device.py は 1〜10 を使用）。
SERVER_SESSION_ID = 0xFF
TEST_SESSION_ID   = 0xFE   # テストペイロード専用（デバイス側で検証処理）
TEST_PAYLOAD_SIZE = 32     # パターン長: [0x00, 0x01, ..., 0x1F]

_tone_task: asyncio.Task | None = None


def is_running() -> bool:
    return _tone_task is not None and not _tone_task.done()


def _send_ptt_notify(group_id: int) -> None:
    """グループ内全端末に MSG_PTT_NOTIFY を送信する（サーバー発）。"""
    ctrl = ctrl_server.get_transport()
    if ctrl is None:
        logger.warning("PTT_NOTIFY: ctrl transport not ready")
        return
    pkt = protocol.make_ptt_notify(SERVER_SESSION_ID)
    for d in device.all_in_group(group_id):
        if d.ctrl_addr:
            ctrl.sendto(pkt, d.ctrl_addr)
    monitor.log("INFO", "server", f"PTT_NOTIFY sent to group={group_id} (session={SERVER_SESSION_ID:#04x})")


def _send_ptt_notify_stop(group_id: int) -> None:
    """グループ内全端末に MSG_PTT_NOTIFY_STOP を送信する。"""
    ctrl = ctrl_server.get_transport()
    if ctrl is None:
        return
    pkt = protocol.make_ptt_notify_stop()
    for d in device.all_in_group(group_id):
        if d.ctrl_addr:
            ctrl.sendto(pkt, d.ctrl_addr)
    monitor.log("INFO", "server", f"PTT_NOTIFY_STOP sent to group={group_id}")


async def send_tone(transport, group_id: int, duration_sec: int = 5) -> None:
    """1kHz 正弦波を Opus エンコードして指定グループの全端末に送信する。"""
    global _tone_task

    enc = opuslib.Encoder(SAMPLE_RATE, CHANNELS, "voip")
    enc.bitrate = BITRATE

    phase     = 0.0
    phase_inc = 2.0 * math.pi * TONE_FREQ_HZ / SAMPLE_RATE
    seq       = 0
    ts        = 0
    frames    = duration_sec * (1000 // 20)  # 20ms/frame → 50 frames/sec

    logger.info(f"Tone start: group={group_id} duration={duration_sec}s ({frames} frames)")

    # PTT 開始を端末に通知し、EVT_FLOOR_BUSY を立てさせる
    _send_ptt_notify(group_id)
    await asyncio.sleep(0.050)  # 端末が PTT_NOTIFY を処理するまで待機

    for _ in range(frames):
        # 1フレーム分の 1kHz サイン波を生成
        pcm = []
        for _ in range(FRAME_SAMPLES):
            s = int(TONE_AMP * math.sin(phase))
            pcm.append(max(-32768, min(32767, s)))
            phase += phase_inc
            if phase >= 2.0 * math.pi:
                phase -= 2.0 * math.pi

        pcm_bytes  = struct.pack(f'<{FRAME_SAMPLES}h', *pcm)
        opus_bytes = enc.encode(pcm_bytes, FRAME_SAMPLES)
        opus_len   = len(opus_bytes)

        # UDPヘッダー（8バイト、ESP32のpacked struct と同じリトルエンディアン）
        flags = (protocol.UDP_TYPE_AUDIO << 7) | (group_id & 0x0F)
        pkt   = struct.pack('<BBHHH', SERVER_SESSION_ID, flags, seq, ts, opus_len) + opus_bytes

        for target in device.all_in_group(group_id):
            addr = device.get_audio_addr(target.session_id)
            if addr:
                try:
                    transport.sendto(pkt, addr)
                except Exception as e:
                    logger.warning(f"Tone sendto {addr} failed: {e}")

        seq = (seq + 1) & 0xFFFF
        ts  = (ts + 20) & 0xFFFF

        await asyncio.sleep(0.020)

    # PTT 終了を端末に通知
    _send_ptt_notify_stop(group_id)
    logger.info(f"Tone done: group={group_id}")
    _tone_task = None


def start(transport, group_id: int, duration_sec: int = 5) -> bool:
    """トーン送信タスクを開始する。既に実行中なら False を返す。"""
    global _tone_task
    if is_running():
        logger.warning("Tone already running")
        return False
    _tone_task = asyncio.ensure_future(send_tone(transport, group_id, duration_sec))
    return True


async def send_test_payload(transport, group_id: int, count: int = 50) -> None:
    """既知パターン [0x00..0x1F] を count パケット送信する。
    デバイスは受信バイト列をパターンと比較し TEST PASS/FAIL をシリアルログに出力する。
    """
    pattern = bytes(range(TEST_PAYLOAD_SIZE))  # [0x00, 0x01, ..., 0x1F]
    flags   = (protocol.UDP_TYPE_AUDIO << 7) | (group_id & 0x0F)

    monitor.log("INFO", "server",
                f"Test payload start: group={group_id} count={count} "
                f"size={TEST_PAYLOAD_SIZE}B pattern=[{' '.join(f'{b:02X}' for b in pattern[:8])}...]")
    logger.info(f"Test payload start: group={group_id} count={count}")

    # PTT 開始を端末に通知
    _send_ptt_notify(group_id)
    await asyncio.sleep(0.050)

    sent = 0
    for seq in range(count):
        pkt = struct.pack('<BBHHH', TEST_SESSION_ID, flags, seq, 0, len(pattern)) + pattern

        for target in device.all_in_group(group_id):
            addr = device.get_audio_addr(target.session_id)
            if addr:
                try:
                    transport.sendto(pkt, addr)
                    sent += 1
                except Exception as e:
                    logger.warning(f"Test sendto {addr} failed: {e}")

        await asyncio.sleep(0.020)

    # PTT 終了を端末に通知
    _send_ptt_notify_stop(group_id)
    monitor.log("INFO", "server",
                f"Test payload done: group={group_id} sent={sent} packets")
    logger.info(f"Test payload done: group={group_id} sent={sent}")
    _tone_task = None


def start_test(transport, group_id: int, count: int = 50) -> bool:
    """テストペイロード送信タスクを開始する。既に実行中なら False を返す。"""
    global _tone_task
    if is_running():
        logger.warning("Tone/test already running")
        return False
    _tone_task = asyncio.ensure_future(send_test_payload(transport, group_id, count))
    return True
