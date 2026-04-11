"""
websockets ライブラリを使った WebSocket サーバー（127.0.0.1:8080）。
静的ファイル配信は nginx が担当するため、このサーバーは /ws のみ提供する。
"""
import json
import logging
import time

import websockets
import websockets.exceptions

import device as dev_module
import monitor
import audio_server
import tone_sender

logger = logging.getLogger("web")


async def ws_handler(websocket) -> None:
    monitor.add_client(websocket)
    try:
        # ---- 初期配信 ----
        # 1. スナップショット（現在の全端末状態）
        await websocket.send(json.dumps({"type": "snapshot", "devices": _device_list()}))

        # 2. 直近のログ
        for entry in monitor.recent_logs():
            await websocket.send(json.dumps(entry))

        # 3. 音声デコーダ初期化情報
        await websocket.send(json.dumps(
            {"type": "audio_init", "sample_rate": 16000, "channels": 1}))

        # ---- クライアントメッセージ受信ループ ----
        async for msg in websocket:
            try:
                data   = json.loads(msg)
                action = data.get("action")
                if action == "subscribe_audio":
                    monitor.subscribe_audio(websocket)
                elif action == "unsubscribe_audio":
                    monitor.unsubscribe_audio(websocket)
                elif action == "send_tone":
                    group_id    = int(data.get("group", 1))
                    duration    = int(data.get("duration", 5))
                    transport   = audio_server.get_transport()
                    if transport is None:
                        logger.warning("send_tone: audio transport not ready")
                    elif tone_sender.is_running():
                        logger.warning("send_tone: already running")
                    else:
                        monitor.log("INFO", "server",
                                    f"Tone start: group={group_id} duration={duration}s")
                        tone_sender.start(transport, group_id, duration)
                elif action == "send_test":
                    group_id  = int(data.get("group", 1))
                    count     = int(data.get("count", 50))
                    transport = audio_server.get_transport()
                    if transport is None:
                        logger.warning("send_test: audio transport not ready")
                    elif tone_sender.is_running():
                        logger.warning("send_test: already running")
                    else:
                        tone_sender.start_test(transport, group_id, count)
            except Exception:
                pass

    except websockets.exceptions.ConnectionClosed:
        pass
    except Exception as e:
        logger.error(f"WS handler error: {e}")
    finally:
        monitor.remove_client(websocket)


def _device_list() -> list:
    return [
        {
            "session_id":   d.session_id,
            "device_id":    f"0x{d.device_id:08X}",
            "group_id":     d.group_id,
            "status":       d.status,
            "connected_at": time.strftime("%H:%M:%S", time.localtime(d.connected_at)),
            "last_seen":    time.strftime("%H:%M:%S", time.localtime(d.last_seen)),
        }
        for d in dev_module.all_devices()
    ]
