"""A load-test batch: one recording replayed by many bots at shifted positions."""

import asyncio
import contextlib
import logging
import time
from collections.abc import Awaitable, Callable
from dataclasses import dataclass
from datetime import UTC, datetime
from enum import StrEnum
from typing import Any

from server.loadtest.plan import BotPlan, plan_bots
from server.playback.publisher import Publisher
from server.playback.session import PlaybackSession, PlaybackStatus
from server.protocol import MAXIMUM_NAME_BYTES
from server.recording.format import Recording

logger = logging.getLogger("uvicorn.error")

# Creates bot ``index``'s connection to the players and returns its player ID.
Spawn = Callable[[int], Awaitable[tuple[str, Publisher]]]


class Transport(StrEnum):
    # Replayed inside the server, like a replay from the panel.
    VIRTUAL = "virtual"
    # Connected as a loopback Socket.IO client, through the player ingestion path.
    SOCKET = "socket"


class BatchStatus(StrEnum):
    STARTING = "starting"
    RUNNING = "running"
    PAUSED = "paused"


class BotStatus(StrEnum):
    PENDING = "pending"
    PLAYING = "playing"
    PAUSED = "paused"
    FAILED = "failed"


@dataclass(frozen=True)
class BatchSettings:
    players: int
    transport: Transport
    seed: int
    jitter: float
    rate_spread: float
    ramp_up_seconds: float


@dataclass(frozen=True)
class BotState:
    index: int
    name: str
    player_id: str | None
    status: BotStatus
    start_ms: int
    rate: float
    position_ms: int
    error: str | None


@dataclass(frozen=True)
class BatchState:
    id: str
    recording_id: str
    label: str
    player_name: str
    created_at: str
    duration_ms: int
    status: BatchStatus
    settings: BatchSettings
    bots: list[BotState]


def bot_name(base: str, index: int) -> str:
    """``base`` numbered from 1, shortened to fit the protocol's name limit."""
    suffix = f" #{index + 1}"
    budget = MAXIMUM_NAME_BYTES - len(suffix.encode())
    return base.encode()[:budget].decode(errors="ignore") + suffix


class _RenamedPublisher:
    """Gives a bot its own name, so players can tell the bots apart."""

    def __init__(self, inner: Publisher, name: str) -> None:
        self._inner = inner
        self._name = name

    async def checkpoint(self, payload: dict[str, Any]) -> None:
        await self._inner.checkpoint(payload)

    async def action(self, payload: dict[str, Any]) -> None:
        await self._inner.action(payload)

    async def mount_transition(self, payload: dict[str, Any]) -> None:
        await self._inner.mount_transition(payload)

    async def appearance(self, payload: dict[str, Any]) -> None:
        await self._inner.appearance({**payload, "name": self._name})

    async def equipment(self, payload: dict[str, Any]) -> None:
        await self._inner.equipment(payload)

    async def exit(self) -> None:
        await self._inner.exit()

    async def left(self) -> None:
        await self._inner.left()


@dataclass
class _Bot:
    plan: BotPlan
    name: str
    player_id: str | None = None
    session: PlaybackSession | None = None
    error: str | None = None


class LoadBatch:
    """Bots that each replay the same recording from their own offset, looping.

    Bots join one by one over the ramp-up. Each is an ordinary playback session,
    so it numbers its own messages and holds its pose while paused. A bot that
    cannot join or whose replay fails is reported and leaves the others running.
    """

    def __init__(
        self,
        batch_id: str,
        recording: Recording,
        settings: BatchSettings,
        spawn: Spawn,
        *,
        clock: Callable[[], float] = time.monotonic,
        on_change: Callable[[], None] = lambda: None,
    ) -> None:
        self.id = batch_id
        self.settings = settings
        self._recording = recording
        self._spawn = spawn
        self._clock = clock
        self._on_change = on_change
        self._created_at = datetime.now(UTC).isoformat(timespec="seconds")
        base_name = recording.header.player_name or "Bot"
        plans = plan_bots(
            settings.players,
            recording.duration_ms,
            seed=settings.seed,
            jitter=settings.jitter,
            rate_spread=settings.rate_spread,
            ramp_up_seconds=settings.ramp_up_seconds,
        )
        self._bots = [_Bot(plan, bot_name(base_name, plan.index)) for plan in plans]
        self._playing = True
        self._task: asyncio.Task[None] | None = None

    # -- observation -------------------------------------------------------

    @property
    def status(self) -> BatchStatus:
        if not self._playing:
            return BatchStatus.PAUSED
        if any(bot.session is None and bot.error is None for bot in self._bots):
            return BatchStatus.STARTING
        return BatchStatus.RUNNING

    def player_ids(self) -> list[str]:
        return [bot.player_id for bot in self._bots if bot.player_id is not None]

    def state(self) -> BatchState:
        header = self._recording.header
        return BatchState(
            id=self.id,
            recording_id=header.id,
            label=header.label,
            player_name=header.player_name,
            created_at=self._created_at,
            duration_ms=self._recording.duration_ms,
            status=self.status,
            settings=self.settings,
            bots=[self._bot_state(bot) for bot in self._bots],
        )

    def _bot_state(self, bot: _Bot) -> BotState:
        session = bot.session
        error = bot.error or (session.error if session is not None else None)
        if error is not None:
            status = BotStatus.FAILED
        elif session is None:
            status = BotStatus.PENDING
        elif session.status is PlaybackStatus.PLAYING:
            status = BotStatus.PLAYING
        else:
            status = BotStatus.PAUSED
        return BotState(
            index=bot.plan.index,
            name=bot.name,
            player_id=bot.player_id,
            status=status,
            start_ms=bot.plan.start_ms,
            rate=bot.plan.rate,
            position_ms=round(session.position_ms()) if session is not None else bot.plan.start_ms,
            error=error,
        )

    # -- control -----------------------------------------------------------

    def start(self) -> None:
        self._task = asyncio.get_running_loop().create_task(
            self._ramp_up(), name=f"load-batch-{self.id}"
        )

    def pause(self) -> None:
        self._playing = False
        for session in self._sessions():
            session.pause()
        self._on_change()

    def resume(self) -> None:
        self._playing = True
        for session in self._sessions():
            # A failed session has no loop left to play it.
            if session.error is None:
                session.play()
        self._on_change()

    async def close(self) -> None:
        """Stop joining and remove every bot from the players."""
        task, self._task = self._task, None
        if task is not None:
            task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await task
        results = await asyncio.gather(
            *(session.close() for session in self._sessions()), return_exceptions=True
        )
        for result in results:
            if isinstance(result, Exception):
                logger.warning("Load batch %s: a bot did not close cleanly: %s", self.id, result)

    # -- ramp-up -----------------------------------------------------------

    def _sessions(self) -> list[PlaybackSession]:
        return [bot.session for bot in self._bots if bot.session is not None]

    async def _ramp_up(self) -> None:
        started = self._clock()
        for bot in self._bots:
            delay = started + bot.plan.join_delay_seconds - self._clock()
            if delay > 0:
                await asyncio.sleep(delay)
            await self._join(bot)
        logger.info("Load batch %s: all %d bots joined", self.id, len(self._bots))

    async def _join(self, bot: _Bot) -> None:
        plan = bot.plan
        try:
            player_id, publisher = await self._spawn(plan.index)
        except Exception as error:
            logger.warning("Load batch %s: bot %d could not join: %s", self.id, plan.index + 1, error)
            bot.error = str(error) or type(error).__name__
            self._on_change()
            return
        bot.player_id = player_id
        bot.session = PlaybackSession(
            f"{self.id}-{plan.index + 1}",
            player_id,
            self._recording,
            _RenamedPublisher(publisher, bot.name),
            autoplay=self._playing,
            rate=plan.rate,
            loop=True,
            start_ms=plan.start_ms,
            clock=self._clock,
            on_change=self._on_change,
        )
        bot.session.start()
        self._on_change()
