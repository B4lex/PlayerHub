"""Pushes admin state to connected panels, coalescing bursts of changes."""

import asyncio
import logging
import time
from collections.abc import Awaitable, Callable
from typing import Any

logger = logging.getLogger("uvicorn.error")


class StateBroadcaster:
    """Sends a fresh state snapshot after changes, at most once per interval.

    ``changed()`` is safe to call as often as the sync path likes (it is called
    for every accepted player update): it costs nothing when no panel is open or
    a send is already scheduled. The first change after a quiet period is sent
    immediately, so a click in the panel does not wait; later changes within the
    interval collapse into one trailing send that carries the latest state.
    """

    def __init__(
        self,
        build: Callable[[], Any],
        publish: Callable[[Any], Awaitable[None]],
        min_interval: float = 0.5,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self._build = build
        self._publish = publish
        self._min_interval = min_interval
        self._clock = clock
        self._listeners = 0
        self._last_sent = float("-inf")
        self._scheduled: asyncio.TimerHandle | None = None
        self._tasks: set[asyncio.Task[None]] = set()

    def listener_added(self) -> None:
        self._listeners += 1

    def listener_removed(self) -> None:
        self._listeners = max(0, self._listeners - 1)

    def changed(self) -> None:
        if self._listeners == 0 or self._scheduled is not None:
            return
        try:
            loop = asyncio.get_running_loop()
        except RuntimeError:
            return  # not on the event loop, so there is nothing that could be sending
        delay = max(0.0, self._last_sent + self._min_interval - self._clock())
        self._scheduled = loop.call_later(delay, self._send)

    def _send(self) -> None:
        self._scheduled = None
        self._last_sent = self._clock()
        task = asyncio.get_running_loop().create_task(self._publish_snapshot())
        self._tasks.add(task)
        task.add_done_callback(self._tasks.discard)

    async def _publish_snapshot(self) -> None:
        try:
            await self._publish(self._build())
        except Exception:
            logger.exception("Could not publish admin state")
