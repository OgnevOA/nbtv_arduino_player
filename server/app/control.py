"""Control hub: job model, command queue (long-poll), and device status.

Single-user, single-device. The bot pushes commands; the device long-polls for
them and posts status back. Commands carry a monotonic sequence number so the
device can ask for "anything newer than seq N".
"""

from __future__ import annotations

import asyncio
import time
from dataclasses import dataclass, field
from typing import Any


@dataclass
class Job:
    id: str                      # cache key; device fetches /stream?id=<id>
    title: str                   # human label for /status
    frames: int
    loop: bool = False


class Hub:
    def __init__(self, *, history: int = 64, offline_after: float = 8.0):
        self._seq = 0
        self._commands: list[dict[str, Any]] = []
        self._history = history
        self._event = asyncio.Event()
        self.current: Job | None = None
        self.nxt: Job | None = None
        self._status: dict[str, Any] = {}
        self._status_ts = 0.0
        self._offline_after = offline_after

    # --- command production (bot side) ------------------------------------
    def _push(self, **cmd: Any) -> int:
        self._seq += 1
        cmd["seq"] = self._seq
        self._commands.append(cmd)
        if len(self._commands) > self._history:
            self._commands = self._commands[-self._history:]
        self._event.set()
        self._event = asyncio.Event()  # wake current waiters, reset for next
        return self._seq

    def play(self, job: Job) -> int:
        self.current = job
        self.nxt = None
        return self._push(cmd="play", id=job.id, loop=job.loop, title=job.title)

    def enqueue_next(self, job: Job) -> int:
        """Replace the 1-deep next slot (newest wins)."""
        self.nxt = job
        return self._push(cmd="preload", id=job.id)

    def stop(self) -> int:
        self.current = None
        return self._push(cmd="stop")

    def skip(self) -> int:
        if self.nxt:
            return self.play(self.nxt)
        return self.stop()

    def freeze(self) -> int:
        return self._push(cmd="freeze")

    def set_loop(self, loop: bool) -> int:
        if self.current:
            self.current.loop = loop
        return self._push(cmd="loop", value=loop)

    def set_speed(self, value: float) -> int:
        return self._push(cmd="speed", value=round(value, 4))

    def reboot(self) -> int:
        return self._push(cmd="reboot")

    # --- command consumption (device long-poll) ---------------------------
    async def poll(self, since: int, timeout: float = 25.0) -> dict[str, Any] | None:
        """Return the first command with seq > since, waiting up to timeout."""
        deadline = time.monotonic() + timeout
        while True:
            for c in self._commands:
                if c["seq"] > since:
                    return c
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            try:
                await asyncio.wait_for(self._event.wait(), timeout=remaining)
            except asyncio.TimeoutError:
                return None

    @property
    def seq(self) -> int:
        return self._seq

    # --- device status (device -> server) ---------------------------------
    def update_status(self, status: dict[str, Any]) -> None:
        self._status = status
        self._status_ts = time.monotonic()

    @property
    def online(self) -> bool:
        return (time.monotonic() - self._status_ts) < self._offline_after

    def status_text(self) -> str:
        if not self._status_ts:
            return "device: never seen"
        s = self._status
        age = time.monotonic() - self._status_ts
        state = "online" if self.online else f"offline ({age:.0f}s)"
        parts = [f"device: {state}"]
        for k in ("state", "id", "frame_seq", "buffer_ms", "underruns",
                  "rssi", "speed"):
            if k in s:
                parts.append(f"{k}={s[k]}")
        return "  ".join(parts)
