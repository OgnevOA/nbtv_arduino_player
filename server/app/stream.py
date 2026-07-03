"""HTTP handlers: the pixel-frame stream, control long-poll, and status.

The device flow:
  GET  /stream?id=<job>       -> chunked .nbtvf body (loops server-side if asked)
  GET  /control/poll?since=N  -> long-poll; returns the next command as JSON
  POST /status                -> device pushes its status JSON (~1 Hz)
  GET  /health                -> liveness
"""

from __future__ import annotations

import asyncio
from pathlib import Path

from aiohttp import web

from . import nbtv
from .config import settings
from .control import Hub

CHUNK = 64 * 1024


def _authed(request: web.Request) -> bool:
    if not settings.device_token:
        return True
    return request.headers.get("X-Device-Token") == settings.device_token


def job_path(job_id: str) -> Path:
    return settings.cache_dir / f"{job_id}.nbtvf"


async def handle_stream(request: web.Request) -> web.StreamResponse:
    if not _authed(request):
        raise web.HTTPForbidden()
    job_id = request.query.get("id", "")
    hub: Hub = request.app["hub"]
    path = job_path(job_id)
    if not job_id or not path.exists():
        raise web.HTTPNotFound(text="unknown job id")

    loop = request.query.get("loop") == "1" or (
        hub.current is not None and hub.current.id == job_id and hub.current.loop
    )

    resp = web.StreamResponse(
        status=200,
        headers={"Content-Type": "application/octet-stream",
                 "Cache-Control": "no-store"},
    )
    resp.enable_chunked_encoding()
    await resp.prepare(request)

    try:
        while True:
            with open(path, "rb") as fp:
                # Stream header once per pass; the device tolerates re-headers
                # at loop boundaries because it re-syncs on the frame anchor.
                while True:
                    chunk = fp.read(CHUNK)
                    if not chunk:
                        break
                    await resp.write(chunk)
            if not loop:
                break
    except (ConnectionResetError, asyncio.CancelledError):
        pass
    await resp.write_eof()
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
    app.router.add_get("/stream", handle_stream)
    app.router.add_get("/control/poll", handle_poll)
    app.router.add_post("/status", handle_status)
    app.router.add_get("/health", handle_health)
