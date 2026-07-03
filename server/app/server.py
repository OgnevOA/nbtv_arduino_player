"""Entry point: run the HTTP stream/control server and the Telegram bot together.

Both share one Hub in a single asyncio event loop, packaged in one Docker image.
"""

from __future__ import annotations

import asyncio
import logging

from aiohttp import web

from . import stream
from .bot import create_dispatcher
from .config import settings
from .control import Hub

log = logging.getLogger("nbtv")


async def _run() -> None:
    settings.validate()
    hub = Hub()

    app = web.Application()
    app["hub"] = hub
    stream.add_routes(app)

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, settings.http_host, settings.http_port)
    await site.start()
    log.info("HTTP server on %s:%s", settings.http_host, settings.http_port)

    bot, dp = create_dispatcher(hub)
    try:
        log.info("starting Telegram polling")
        await dp.start_polling(bot)
    finally:
        await runner.cleanup()
        await bot.session.close()


def main() -> None:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    try:
        asyncio.run(_run())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
