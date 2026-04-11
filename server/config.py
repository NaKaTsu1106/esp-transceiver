import os


class Config:
    def __init__(self):
        self.ctrl_port        = int(os.getenv("CTRL_PORT",          "6000"))
        self.udp_port         = int(os.getenv("UDP_PORT",           "6001"))
        self.ws_port          = int(os.getenv("WS_PORT",            "8080"))
        self.heartbeat_timeout = int(os.getenv("HEARTBEAT_TIMEOUT", "75"))
        self.log_max_entries  = int(os.getenv("LOG_MAX_ENTRIES",    "1000"))
        self.log_dir          = os.getenv("LOG_DIR",  "/var/log/transceiver")
        self.domain           = os.getenv("DOMAIN",   "")


cfg = Config()
