#!/usr/bin/env python3
"""IP Transceiver Relay Server — エントリポイント"""
import asyncio
import logging
import sys
import time

import websockets

import config
import device
import floor
import monitor
from audio_server import AudioProtocol
from ctrl_server import CtrlProtocol
from web_server import ws_handler

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(name)s] %(message)s",
    stream=sys.stdout,
)
logger = logging.getLogger("main")


async def reaper_task(timeout_sec: int) -> None:
    """タイムアウト端末を 10 秒ごとに削除する。"""
    while True:
        await asyncio.sleep(10)
        now = time.time()
        for d in device.all_devices():
            if now - d.last_seen > timeout_sec:
                logger.info(
                    f"Timeout: session={d.session_id} device=0x{d.device_id:08X} "
                    f"(last seen {now - d.last_seen:.0f}s ago)"
                )
                monitor.log("WARN", f"0x{d.device_id:08X}",
                            f"Session timeout session={d.session_id}")
                if floor.holder(d.group_id) == d.session_id:
                    floor.release(d.group_id, d.session_id)
                device.remove(d.session_id)
                await monitor.broadcast_device_update()


async def main() -> None:
    cfg = config.cfg
    logger.info(
        f"Starting IP Transceiver Server "
        f"(CTRL:{cfg.ctrl_port} UDP:{cfg.udp_port} WS:{cfg.ws_port})"
    )

    monitor.init_file_logger(cfg.log_dir)
    loop = asyncio.get_running_loop()

    # UDP 制御サーバー（ポート 6000）
    ctrl_transport, _ = await loop.create_datagram_endpoint(
        CtrlProtocol,
        local_addr=("0.0.0.0", cfg.ctrl_port),
    )

    # UDP 音声サーバー（ポート 6001）
    audio_transport, _ = await loop.create_datagram_endpoint(
        AudioProtocol,
        local_addr=("0.0.0.0", cfg.udp_port),
    )

    # タイムアウト監視タスク
    asyncio.create_task(reaper_task(cfg.heartbeat_timeout))

    # WebSocket サーバー（ポート 8080）
    ws_server = await websockets.serve(ws_handler, "0.0.0.0", cfg.ws_port)
    logger.info(f"WebSocket server : ws://0.0.0.0:{cfg.ws_port}")

    try:
        await asyncio.Future()  # 永続実行
    except (asyncio.CancelledError, KeyboardInterrupt):
        pass
    finally:
        ctrl_transport.close()
        audio_transport.close()
        ws_server.close()
        await ws_server.wait_closed()
        logger.info("Server stopped.")


if __name__ == "__main__":
    asyncio.run(main())
