"""HTTP handlers: the PCM radio stream, control long-poll, and status.

The device flow:
  GET  /radio.wav             -> endless mono s16le PCM (WAV header + body)
  GET  /control/poll?since=N  -> long-poll; returns the next device command
  POST /status                -> device pushes its status JSON (~1 Hz)
  GET  /health                -> liveness

/radio.wav always streams: the current program's PCM (looped), or a test-card
loop when idle. It switches source seamlessly when the program token changes,
and TCP backpressure paces the sender to the device's playback rate. The 44-byte
WAV header also makes it directly playable in ffplay/VLC for debugging.
"""

from __future__ import annotations

import asyncio
from pathlib import Path

from aiohttp import web

from . import nbtv
from .config import settings
from .control import Hub

CHUNK = 32 * 1024


def _authed(request: web.Request) -> bool:
    if not settings.device_token:
        return True
    return request.headers.get("X-Device-Token") == settings.device_token


async def _write_bytes(resp: web.StreamResponse, data: bytes,
                       hub: Hub, token: int) -> bool:
    """Write a whole buffer in CHUNKs. Return False if the program changed."""
    for i in range(0, len(data), CHUNK):
        await resp.write(data[i:i + CHUNK])
        if hub.program_token != token:
            return False
    return True


async def _write_file(resp: web.StreamResponse, path: Path,
                      hub: Hub, token: int) -> bool:
    """Stream a PCM file in CHUNKs. Return False if the program changed."""
    with open(path, "rb") as fp:
        while True:
            chunk = fp.read(CHUNK)
            if not chunk:
                return True
            await resp.write(chunk)
            if hub.program_token != token:
                return False


async def handle_radio(request: web.Request) -> web.StreamResponse:
    if not _authed(request):
        raise web.HTTPForbidden()
    hub: Hub = request.app["hub"]
    testcard: bytes = request.app["testcard_pcm"]

    resp = web.StreamResponse(
        headers={"Content-Type": "audio/wav", "Cache-Control": "no-store"},
    )
    resp.enable_chunked_encoding()
    await resp.prepare(request)
    await resp.write(nbtv.wav_stream_header())

    try:
        while True:
            token, path, loop = hub.program_source()
            if path is None or not path.exists():
                # Idle: loop the test card until the program changes.
                while hub.program_token == token:
                    if not await _write_bytes(resp, testcard, hub, token):
                        break
            else:
                # Play the program's PCM, looping if requested.
                while hub.program_token == token:
                    if not await _write_file(resp, path, hub, token):
                        break
                    if hub.program_token != token:
                        break
                    if not loop:
                        hub.stop()   # finite item ended -> go idle (test card)
                        break
    except (ConnectionResetError, asyncio.CancelledError):
        pass
    return resp


async def handle_poll(request: web.Request) -> web.Response:
    if not _authed(request):
        raise web.HTTPForbidden()
    hub: Hub = request.app["hub"]
    try:
        since = int(request.query.get("since", "0"))
    except ValueError:
        since = 0
    cmd = await hub.poll(since)
    if cmd is None:
        return web.json_response({"cmd": "none", "seq": hub.seq})
    return web.json_response(cmd)


async def handle_status(request: web.Request) -> web.Response:
    if not _authed(request):
        raise web.HTTPForbidden()
    hub: Hub = request.app["hub"]
    try:
        data = await request.json()
    except Exception:
        data = {}
    hub.update_status(data)
    return web.json_response({"ok": True, "seq": hub.seq})


async def handle_health(request: web.Request) -> web.Response:
    return web.json_response({"ok": True})


def add_routes(app: web.Application) -> None:
    app.router.add_get("/radio.wav", handle_radio)
    app.router.add_get("/control/poll", handle_poll)
    app.router.add_post("/status", handle_status)
    app.router.add_get("/health", handle_health)
