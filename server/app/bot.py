"""Telegram bot (aiogram v3), single-user allow-list.

Send a YouTube URL, a video, or a GIF and it is encoded to a .nbtvf and played
on the device. Slash commands control playback.
"""

from __future__ import annotations

import asyncio
import tempfile
from pathlib import Path

from aiogram import Bot, Dispatcher, F
from aiogram.filters import Command, CommandObject
from aiogram.types import Message

from . import encoder
from .config import settings
from .control import Hub, Job

HELP = (
    "Send a YouTube URL, a video, or a GIF to play it on the NBTV set.\n\n"
    "/stop - stop playback\n"
    "/skip - play the queued item\n"
    "/loop on|off - loop the current item\n"
    "/speed 0.95 - set disc speed\n"
    "/status - device status\n"
    "/test - show the device test card"
)


def _allowed(message: Message) -> bool:
    return bool(message.from_user) and message.from_user.id == settings.allowed_user_id


async def _encode_job(source: str, title: str, *, loop: bool) -> Job:
    """Encode (blocking) in a thread; reuse cache when present."""
    opt = encoder.EncodeOptions(max_height=settings.default_max_height)
    key = opt.cache_key(source)
    out = settings.cache_dir / f"{key}.nbtvf"

    def work() -> int:
        if out.exists():
            # count frames cheaply from file size
            size = out.stat().st_size
            per = encoder.nbtv.FRAME_HEADER_SIZE + encoder.nbtv.FRAME_BYTES
            return max(0, (size - encoder.nbtv.STREAM_HEADER_SIZE) // per)
        with tempfile.TemporaryDirectory(prefix="nbtv-") as td:
            return encoder.encode_to_nbtvf(source, out, opt, Path(td))

    frames = await asyncio.get_running_loop().run_in_executor(None, work)
    return Job(id=key, title=title, frames=frames, loop=loop)


def create_dispatcher(hub: Hub) -> tuple[Bot, Dispatcher]:
    bot = Bot(token=settings.bot_token)
    dp = Dispatcher()

    @dp.message(Command("start", "help"))
    async def cmd_help(message: Message):
        if not _allowed(message):
            return
        await message.answer(HELP)

    @dp.message(Command("stop"))
    async def cmd_stop(message: Message):
        if not _allowed(message):
            return
        hub.stop()
        await message.answer("stopped")

    @dp.message(Command("skip"))
    async def cmd_skip(message: Message):
        if not _allowed(message):
            return
        hub.skip()
        await message.answer("skipped")

    @dp.message(Command("loop"))
    async def cmd_loop(message: Message, command: CommandObject):
        if not _allowed(message):
            return
        on = (command.args or "").strip().lower() in ("on", "1", "true", "yes")
        hub.set_loop(on)
        await message.answer(f"loop {'on' if on else 'off'}")

    @dp.message(Command("speed"))
    async def cmd_speed(message: Message, command: CommandObject):
        if not _allowed(message):
            return
        try:
            value = float((command.args or "").strip())
        except ValueError:
            await message.answer("usage: /speed 0.95")
            return
        hub.set_speed(value)
        await message.answer(f"speed set to {value:.4f}")

    @dp.message(Command("status"))
    async def cmd_status(message: Message):
        if not _allowed(message):
            return
        await message.answer(hub.status_text())

    @dp.message(Command("test"))
    async def cmd_test(message: Message):
        if not _allowed(message):
            return
        hub._push(cmd="testcard")
        await message.answer("showing test card")

    @dp.message(F.video | F.animation | F.document)
    async def on_media(message: Message):
        if not _allowed(message):
            return
        obj = message.video or message.animation or message.document
        await message.answer("downloading...")
        with tempfile.TemporaryDirectory(prefix="nbtv-dl-") as td:
            local = Path(td) / "input"
            await bot.download(obj, destination=local)
            await message.answer("encoding...")
            job = await _encode_job(str(local), "upload", loop=False)
        hub.play(job)
        await message.answer(f"playing ({job.frames} frames)")

    @dp.message(F.text)
    async def on_text(message: Message):
        if not _allowed(message):
            return
        text = (message.text or "").strip()
        if not encoder.is_url(text):
            await message.answer("send a URL, video, or GIF (or /help)")
            return
        await message.answer("downloading + encoding...")
        job = await _encode_job(text, text, loop=False)
        hub.play(job)
        await message.answer(f"playing ({job.frames} frames)")

    return bot, dp
