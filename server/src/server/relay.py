"""Relays player checkpoints to every client in one batch per tick."""

import asyncio
import contextlib
import logging
from collections.abc import Awaitable, Callable
from typing import Any

logger = logging.getLogger("uvicorn.error")

# Twice the clients' 20 Hz checkpoint rate, so an update waits at most one
# short tick and two consecutive updates of a player rarely share a tick.
TICK_INTERVAL_SECONDS = 0.025

Emit = Callable[[str, dict[str, Any]], Awaitable[Any]]


class CheckpointRelay:
    """Holds the newest unsent checkpoint of every player and broadcasts them together.

    A broadcast without per-recipient exclusions is encoded once for all
    clients, so each batch also reaches the players it contains; clients skip
    their own entry by the ID the server sends them in ``session:welcome``.

    Receivers keep only a player's latest checkpoint, so a newer one replaces
    an unsent one. Messages that name a checkpoint sequence (actions, mount
    transitions) must not overtake it: call ``flush_player`` before relaying
    them, and ``discard`` before telling peers the player has left.
    """

    def __init__(self, emit: Emit, interval: float = TICK_INTERVAL_SECONDS) -> None:
        self._emit = emit
        self._interval = interval
        self._pending: dict[str, dict[str, Any]] = {}
        self._task: asyncio.Task[None] | None = None

    def publish(self, player_id: str, outbound: dict[str, Any]) -> None:
        self._pending[player_id] = outbound

    def discard(self, player_id: str) -> None:
        self._pending.pop(player_id, None)

    async def flush_player(self, player_id: str) -> None:
        state = self._pending.pop(player_id, None)
        if state is not None:
            await self._send([state])

    async def flush(self) -> None:
        if not self._pending:
            return
        states = list(self._pending.values())
        self._pending.clear()
        await self._send(states)

    def start(self) -> None:
        if self._task is None or self._task.done():
            self._task = asyncio.get_running_loop().create_task(self._run())

    async def stop(self) -> None:
        if self._task is None:
            return
        self._task.cancel()
        with contextlib.suppress(asyncio.CancelledError):
            await self._task
        self._task = None

    async def _send(self, states: list[dict[str, Any]]) -> None:
        logger.debug("Socket.IO TX player:states count=%s", len(states))
        await self._emit("player:states", {"states": states})

    async def _run(self) -> None:
        loop = asyncio.get_running_loop()
        deadline = loop.time()
        while True:
            # Scheduled against a fixed deadline so the flush time does not
            # stretch the tick; after a stall the cadence restarts from now
            # instead of sending a burst of catch-up batches.
            deadline = max(deadline + self._interval, loop.time())
            await asyncio.sleep(deadline - loop.time())
            try:
                await self.flush()
            except Exception:
                logger.exception("Could not relay player checkpoints")
