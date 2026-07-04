"""Telegram bot (aiogram v3), single-user allow-list.

Send a YouTube URL, a video, or a GIF and it is encoded to NBTV PCM and played
on the device (which streams it as a continuous radio). Slash commands control
playback.
"""

from __future__ import annotations

import asyncio
import tempfile
from pathlib import Path

from aiogram import Bot, Dispatcher, F
from aiogram.filters import Command, CommandObject
from aiogram.types import (CallbackQuery, InlineKeyboardButton,
                           InlineKeyboardMarkup, Message)

from . import encoder, nbtv
from .config import settings
from .control import Hub, Program

HELP = (
    "Send a YouTube URL, a video, or a GIF to play it on the NBTV set.\n\n"
    "Use the buttons below, or:\n"
    "/menu - show the control buttons\n"
    "/stop - go idle (test card)\n"
    "/skip - play the queued item\n"
    "/loop on|off - loop the current item\n"
    "/speed 0.95 - set disc speed\n"
    "/invert - flip signal polarity\n"
    "/status - device + program status\n"
    "/test - show the test card"
)


def _controls(hub: Hub) -> InlineKeyboardMarkup:
    """The main inline control panel."""
    loop_on = bool(hub.program and hub.program.loop)
    loop_lbl = "Loop: ON" if loop_on else "Loop: off"
    return InlineKeyboardMarkup(inline_keyboard=[
        [InlineKeyboardButton(text="Stop", callback_data="stop"),
         InlineKeyboardButton(text="Skip", callback_data="skip")],
        [InlineKeyboardButton(text=loop_lbl, callback_data="loop"),
         InlineKeyboardButton(text="Test card", callback_data="test")],
        [InlineKeyboardButton(text="Slower --", callback_data="spd:-0.005"),
         InlineKeyboardButton(text="- ", callback_data="spd:-0.001"),
         InlineKeyboardButton(text="+", callback_data="spd:+0.001"),
         InlineKeyboardButton(text="Faster ++", callback_data="spd:+0.005")],
        [InlineKeyboardButton(text="Invert", callback_data="invert"),
         InlineKeyboardButton(text="Status", callback_data="status")],
    ])


def _allowed(message: Message) -> bool:
    return bool(message.from_user) and message.from_user.id == settings.allowed_user_id


def _allowed_cb(cb: CallbackQuery) -> bool:
    return bool(cb.from_user) and cb.from_user.id == settings.allowed_user_id


async def _encode_program(source: str, title: str, *, loop: bool) -> Program:
    """Encode (blocking) in a thread; reuse the .pcm cache when present."""
    opt = encoder.EncodeOptions(max_height=settings.default_max_height,
                                headroom=settings.default_headroom,
                                lowpass=settings.default_lowpass)
    key = opt.cache_key(source)
    out = settings.cache_dir / f"{key}.pcm"

    def work() -> int:
        if out.exists():
            return out.stat().st_size // (nbtv.BYTES_PER_SAMPLE * nbtv.FRAME_SAMPLES)
        with tempfile.TemporaryDirectory(prefix="nbtv-") as td:
            return encoder.encode_to_pcm(source, out, opt, Path(td))

    frames = await asyncio.get_running_loop().run_in_executor(None, work)
    return Program(id=key, title=title, pcm=out, frames=frames, loop=loop)


def create_dispatcher(hub: Hub) -> tuple[Bot, Dispatcher]:
    bot = Bot(token=settings.bot_token)
    dp = Dispatcher()

    @dp.message(Command("start", "help"))
    async def cmd_help(message: Message):
        if not _allowed(message):
            return
        await message.answer(HELP, reply_markup=_controls(hub))

    @dp.message(Command("menu"))
    async def cmd_menu(message: Message):
        if not _allowed(message):
            return
        await message.answer("NBTV controls:", reply_markup=_controls(hub))

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

    @dp.message(Command("invert"))
    async def cmd_invert(message: Message):
        if not _allowed(message):
            return
        hub.set_invert()
        await message.answer("toggled invert")

    @dp.message(Command("status"))
    async def cmd_status(message: Message):
        if not _allowed(message):
            return
        await message.answer(hub.status_text())

    @dp.message(Command("test"))
    async def cmd_test(message: Message):
        if not _allowed(message):
            return
        hub.stop()  # idle streams the test card
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
            program = await _encode_program(str(local), "upload", loop=True)
        hub.play(program)
        await message.answer(f"playing ({program.frames} frames)",
                             reply_markup=_controls(hub))

    @dp.message(F.text)
    async def on_text(message: Message):
        if not _allowed(message):
            return
        text = (message.text or "").strip()
        if not encoder.is_url(text):
            await message.answer("send a URL, video, or GIF (or /help)")
            return
        await message.answer("downloading + encoding...")
        program = await _encode_program(text, text, loop=True)
        hub.play(program)
        await message.answer(f"playing ({program.frames} frames)",
                             reply_markup=_controls(hub))

    @dp.callback_query()
    async def on_button(cb: CallbackQuery):
        if not _allowed_cb(cb):
            await cb.answer("not allowed", show_alert=True)
            return
        data = cb.data or ""
        toast = ""
        refresh = False
        if data == "stop":
            hub.stop(); toast = "idle (test card)"; refresh = True
        elif data == "skip":
            hub.skip(); toast = "skipped"; refresh = True
        elif data == "loop":
            new = not (hub.program and hub.program.loop)
            hub.set_loop(new); toast = f"loop {'on' if new else 'off'}"; refresh = True
        elif data == "test":
            hub.stop(); toast = "showing test card"; refresh = True
        elif data == "invert":
            hub.set_invert(); toast = "toggled invert"
        elif data == "status":
            toast = "status sent"
            if cb.message:
                await cb.message.answer(hub.status_text())
        elif data.startswith("spd:"):
            try:
                delta = float(data[4:])
            except ValueError:
                delta = 0.0
            new_speed = hub.adjust_speed(delta)
            toast = f"speed {new_speed:.4f}"
        else:
            toast = "?"

        await cb.answer(toast)
        if refresh and cb.message:
            try:
                await cb.message.edit_reply_markup(reply_markup=_controls(hub))
            except Exception:
                pass

    return bot, dp
