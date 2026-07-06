"""Control hub: the radio program, device command queue, and device status.

Single-user, single-device. The bot sets the current *program* (which PCM file
the radio streams, or idle = test card). Program changes bump a monotonic token
that the radio streamer watches so it can switch sources seamlessly.

Device-local knobs that must track the physical disc (speed, invert, reboot)
are still delivered as long-poll commands with a monotonic sequence number.
"""

from __future__ import annotations

import asyncio
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

from . import render
from .config import settings


@dataclass
class Program:
    id: str                 # cache key
    title: str              # human label for /status
    pcm: Path               # path to the cached mono s16le PCM
    frames: int = 0
    loop: bool = True


class Hub:
    def __init__(self, *, history: int = 64, offline_after: float = 8.0,
                 default_speed: float = 0.95, default_gamma: float = 2.2):
        # device command channel (speed / invert / reboot)
        self._seq = 0
        self._commands: list[dict[str, Any]] = []
        self._history = history
        self._event = asyncio.Event()
        self.speed = default_speed          # server-side intent (for keyboard +/-)
        self.gamma = default_gamma          # LED linearization baked into encodes
        self.testcard: bytes = b""          # idle-stream PCM (regenerated on /gamma)
        # radio program channel
        self.program: Program | None = None      # None => idle (test card)
        self.nxt: Program | None = None
        self._ptoken = 0
        self._pevent = asyncio.Event()
        # device status
        self._status: dict[str, Any] = {}
        self._status_ts = 0.0
        self._offline_after = offline_after

    # --- device commands (bot -> device) ----------------------------------
    def _push(self, **cmd: Any) -> int:
        self._seq += 1
        cmd["seq"] = self._seq
        self._commands.append(cmd)
        if len(self._commands) > self._history:
            self._commands = self._commands[-self._history:]
        self._event.set()
        self._event = asyncio.Event()
        return self._seq

    def set_speed(self, value: float) -> int:
        self.speed = round(max(0.80, min(1.20, value)), 4)
        return self._push(cmd="speed", value=self.speed)

    def adjust_speed(self, delta: float) -> float:
        """Nudge speed by delta (for the keyboard +/- buttons). Returns new speed."""
        self.set_speed(self.speed + delta)
        return self.speed

    # --- LED gamma (server-side, baked into encodes) -----------------------
    def render_testcard(self) -> None:
        """(Re)build the idle test-card PCM at the current gamma."""
        self.testcard = render.test_card_pcm(
            headroom=settings.default_headroom, gamma=self.gamma,
            lowpass_hz=settings.default_lowpass)

    def set_gamma(self, value: float) -> float:
        """Set LED gamma, refresh the idle test card, and bump the program token."""
        self.gamma = round(max(0.5, min(4.0, value)), 3)
        self.render_testcard()
        self._bump_program()             # idle stream picks up the new test card
        return self.gamma

    def adjust_gamma(self, delta: float) -> float:
        """Nudge gamma by delta (for the keyboard +/- buttons). Returns new gamma."""
        return self.set_gamma(self.gamma + delta)

    def set_invert(self) -> int:
        return self._push(cmd="invert")

    def reboot(self) -> int:
        return self._push(cmd="reboot")

    async def poll(self, since: int, timeout: float = 25.0) -> dict[str, Any] | None:
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

    # --- radio program (bot -> streamer) ----------------------------------
    def _bump_program(self) -> int:
        self._ptoken += 1
        self._pevent.set()
        self._pevent = asyncio.Event()
        return self._ptoken

    def play(self, program: Program) -> int:
        self.program = program
        self.nxt = None
        return self._bump_program()

    def enqueue_next(self, program: Program) -> None:
        self.nxt = program

    def stop(self) -> int:
        self.program = None
        return self._bump_program()

    def skip(self) -> int:
        if self.nxt:
            p, self.nxt = self.nxt, None
            return self.play(p)
        return self.stop()

    def set_loop(self, loop: bool) -> int:
        if self.program:
            self.program.loop = loop
        return self._bump_program()

    @property
    def program_token(self) -> int:
        return self._ptoken

    def program_source(self) -> tuple[int, Path | None, bool]:
        """(token, pcm path or None for idle/test-card, loop)."""
        if self.program is None:
            return self._ptoken, None, True
        return self._ptoken, self.program.pcm, self.program.loop

    async def wait_program(self, prev_token: int, timeout: float = 25.0) -> None:
        """Return when the program token differs from prev_token (or timeout)."""
        if prev_token != self._ptoken:
            return
        try:
            await asyncio.wait_for(self._pevent.wait(), timeout=timeout)
        except asyncio.TimeoutError:
            pass

    # --- device status (device -> server) ---------------------------------
    def update_status(self, status: dict[str, Any]) -> None:
        self._status = status
        self._status_ts = time.monotonic()

    @property
    def online(self) -> bool:
        return (time.monotonic() - self._status_ts) < self._offline_after

    def status_text(self) -> str:
        prog = self.program.title if self.program else "idle (test card)"
        if not self._status_ts:
            return f"program: {prog}\ndevice: never seen"
        s = self._status
        age = time.monotonic() - self._status_ts
        state = "online" if self.online else f"offline ({age:.0f}s)"
        parts = [f"device: {state}"]
        for k in ("state", "buffer_ms", "underruns", "rssi", "speed"):
            if k in s:
                parts.append(f"{k}={s[k]}")
        return f"program: {prog}\n" + "  ".join(parts)
