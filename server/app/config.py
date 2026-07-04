"""Runtime configuration from environment variables.

Secrets (BOT_TOKEN, ALLOWED_USER_ID) are provided at runtime on TrueNAS and are
never baked into the image.
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path


def _int(name: str, default: int) -> int:
    try:
        return int(os.environ.get(name, default))
    except (TypeError, ValueError):
        return default


@dataclass
class Settings:
    bot_token: str = os.environ.get("BOT_TOKEN", "")
    allowed_user_id: int = _int("ALLOWED_USER_ID", 0)
    device_token: str = os.environ.get("DEVICE_TOKEN", "")  # shared LAN token
    http_host: str = os.environ.get("HTTP_HOST", "0.0.0.0")
    http_port: int = _int("HTTP_PORT", 32125)
    cache_dir: Path = Path(os.environ.get("CACHE_DIR", "/data/cache"))
    default_speed: float = float(os.environ.get("DEFAULT_SPEED", "0.95"))
    default_max_height: int = _int("DEFAULT_MAX_HEIGHT", 360)
    default_headroom: float = float(os.environ.get("DEFAULT_HEADROOM", "0.80"))
    default_lowpass: float = float(os.environ.get("DEFAULT_LOWPASS", "10000"))

    def validate(self) -> None:
        if not self.bot_token:
            raise SystemExit("BOT_TOKEN is required")
        if not self.allowed_user_id:
            raise SystemExit("ALLOWED_USER_ID is required")
        self.cache_dir.mkdir(parents=True, exist_ok=True)


settings = Settings()
