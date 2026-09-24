"""One replay of a recording: a virtual player with play, pause, seek, and speed."""

import asyncio
import contextlib
import logging
import time
from bisect import bisect_right
from collections.abc import Callable
from dataclasses import dataclass
from enum import StrEnum
from typing import Any

from server.playback.publisher import Publisher
from server.playback.virtual_player import VirtualPlayer
from server.recording.format import RecordedMessage, Recording

logger = logging.getLogger("uvicorn.error")

# A paused or finished replay repeats its held checkpoint at this interval so
# that clients joining later still get the actor. Real players publish at 20 Hz;
# a held pose does not change, so a slower rate is enough.
HEARTBEAT_SECONDS = 0.25
MINIMUM_RATE = 0.1
MAXIMUM_RATE = 4.0
# Timeline positions come from float clock arithmetic, so a message due at
# exactly the current position can appear a hair in the future. Treat anything
# within a nanosecond as due instead of waking again for nothing.
_DUE_TOLERANCE_MS = 1e-6


class PlaybackError(ValueError):
    """A recording that cannot be replayed, or an invalid playback request."""


class PlaybackStatus(StrEnum):
    PLAYING = "playing"
    PAUSED = "paused"
    ENDED = "ended"


@dataclass(frozen=True)
class PlaybackState:
    id: str
    recording_id: str
    label: str
    player_id: str
    player_name: str
    status: PlaybackStatus
    position_ms: int
    duration_ms: int
    rate: float
    loop: bool
    # Rises on every change a viewer could see, so stale copies can be told apart.
    revision: int
    error: str | None


class PlaybackSession:
    """Replays one recording as a synthetic peer.

    The timeline position is ``anchor_ms`` at clock time ``anchor_time``, moving
    at ``rate`` while playing. Control methods only change that state and wake
    the run loop; every message is emitted by the loop, which keeps ordering
    simple and lets a seek take effect between two emissions.

    Because checkpoints are latest-wins, a seek does not replay what it skips:
    it sends the appearance, equipment, and checkpoint in effect at the target.
    While the recorded player was out of the world, the virtual player exits it
    too and sends no checkpoints.
    """

    def __init__(
        self,
        session_id: str,
        player_id: str,
        recording: Recording,
        publisher: Publisher,
        *,
        autoplay: bool = True,
        rate: float = 1.0,
        loop: bool = False,
        start_ms: int = 0,
        clock: Callable[[], float] = time.monotonic,
        on_change: Callable[[], None] = lambda: None,
    ) -> None:
        self.id = session_id
        self.player_id = player_id
        self._recording = recording
        self._publisher = publisher
        self._clock = clock
        self._on_change = on_change
        self._virtual = VirtualPlayer()

        self._messages: list[RecordedMessage] = recording.messages
        self._times = [message.t_ms for message in self._messages]
        self._series: dict[str, tuple[list[int], list[dict[str, Any]]]] = {
            event: ([], [])
            for event in ("player:update", "player:appearance", "player:equipment")
        }
        for message in self._messages:
            series = self._series.get(message.event)
            if series is not None:
                series[0].append(message.t_ms)
                series[1].append(message.payload)
        if not self._series["player:update"][0]:
            raise PlaybackError("recording has no player:update checkpoints")
        self._presence_times: list[int] = []
        self._presence: list[bool] = []
        for message in self._messages:
            if message.event in ("player:enter", "player:exit"):
                self._presence_times.append(message.t_ms)
                self._presence.append(message.event == "player:enter")
        self._duration_ms = recording.duration_ms
        self._validate_rate(rate)

        self._playing = autoplay
        self._ended = False
        self._rate = rate
        self._loop = loop
        self._anchor_ms = float(min(max(start_ms, 0), self._duration_ms))
        self._anchor_time = clock()
        self._cursor = 0
        self._snapshot_pending = True
        self._heartbeat_due = 0.0
        self._last_update: dict[str, Any] | None = None
        self._in_world = False
        self._revision = 0
        # Bumped by every control call; an emission in flight checks it after
        # each await and abandons a plan the caller has since changed.
        self._generation = 0
        self._error: str | None = None
        self._wake = asyncio.Event()
        self._task: asyncio.Task[None] | None = None

    # -- observation -------------------------------------------------------

    @property
    def status(self) -> PlaybackStatus:
        if self._playing:
            return PlaybackStatus.PLAYING
        return PlaybackStatus.ENDED if self._ended else PlaybackStatus.PAUSED

    @property
    def in_world(self) -> bool:
        return self._in_world

    def position_ms(self) -> float:
        return self._position(self._clock())

    def state(self) -> PlaybackState:
        header = self._recording.header
        return PlaybackState(
            id=self.id,
            recording_id=header.id,
            label=header.label,
            player_id=self.player_id,
            player_name=header.player_name,
            status=self.status,
            position_ms=round(self._position(self._clock())),
            duration_ms=self._duration_ms,
            rate=self._rate,
            loop=self._loop,
            revision=self._revision,
            error=self._error,
        )

    # -- control -----------------------------------------------------------

    def play(self) -> None:
        if self._playing:
            return
        if self._ended or self._anchor_ms >= self._duration_ms:
            # Like a media player, playing from the end starts over.
            self._anchor_ms = 0.0
            self._snapshot_pending = True
        self._ended = False
        self._playing = True
        self._anchor_time = self._clock()
        self._touch()

    def pause(self) -> None:
        if not self._playing:
            return
        now = self._clock()
        self._anchor_ms = self._position(now)
        self._playing = False
        self._heartbeat_due = now
        self._touch()

    def seek(self, position_ms: float) -> None:
        self._anchor_ms = min(max(position_ms, 0.0), float(self._duration_ms))
        self._anchor_time = self._clock()
        self._ended = False
        self._snapshot_pending = True
        self._touch()

    def configure(self, *, rate: float | None = None, loop: bool | None = None) -> None:
        if rate is not None:
            self._validate_rate(rate)
            now = self._clock()
            self._anchor_ms = self._position(now)
            self._anchor_time = now
            self._rate = rate
        if loop is not None:
            self._loop = loop
        self._touch()

    def start(self) -> None:
        """Run the emission loop on the current event loop."""
        self._task = asyncio.get_running_loop().create_task(
            self._run(), name=f"playback-{self.id}"
        )

    async def close(self) -> None:
        """Stop the loop and remove the virtual player from every client."""
        task, self._task = self._task, None
        if task is not None:
            task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await task
        await self._publisher.left()

    # -- emission loop -----------------------------------------------------

    async def advance(self) -> None:
        """Emit whatever is due now. One pass of the run loop."""
        generation = self._generation
        now = self._clock()

        if self._snapshot_pending:
            self._snapshot_pending = False
            position = self._position(now)
            self._cursor = bisect_right(self._times, position)
            await self._emit_snapshot(position, now)
            if generation != self._generation:
                return

        if self._playing:
            position = self._position(now)
            due_ms = position + _DUE_TOLERANCE_MS
            while self._cursor < len(self._messages) and self._messages[self._cursor].t_ms <= due_ms:
                message = self._messages[self._cursor]
                self._cursor += 1
                await self._emit_message(message)
                if generation != self._generation:
                    return
            if self._cursor >= len(self._messages) and due_ms >= self._duration_ms:
                self._reach_end(now)
        elif now >= self._heartbeat_due:
            self._heartbeat_due = now + HEARTBEAT_SECONDS
            await self._emit_hold()

    def next_delay(self) -> float:
        """Seconds until the loop has something to do."""
        now = self._clock()
        if self._snapshot_pending:
            return 0.0
        if self._playing:
            if self._cursor < len(self._messages):
                due_ms = self._messages[self._cursor].t_ms
            else:
                due_ms = self._duration_ms
            return max(0.0, (due_ms - self._position(now)) / (1000.0 * self._rate))
        return max(0.0, self._heartbeat_due - now)

    async def _run(self) -> None:
        try:
            while True:
                delay = self.next_delay()
                if delay > 0:
                    with contextlib.suppress(TimeoutError):
                        await asyncio.wait_for(self._wake.wait(), timeout=delay)
                self._wake.clear()
                await self.advance()
                await asyncio.sleep(0)  # never starve the event loop, whatever the publisher does
        except asyncio.CancelledError:
            raise
        except Exception as error:
            logger.exception("Playback %s failed", self.id)
            self._error = str(error) or type(error).__name__
            self._playing = False
            self._ended = True
            self._touch()

    async def _emit_snapshot(self, position: float, now: float) -> None:
        self._virtual.break_continuity()
        appearance = self._latest_at("player:appearance", position)
        if appearance is not None and (payload := self._virtual.appearance(appearance)):
            await self._publisher.appearance(payload)
        equipment = self._latest_at("player:equipment", position)
        if equipment is not None and (payload := self._virtual.equipment(equipment)):
            await self._publisher.equipment(payload)
        update = self._checkpoint_at(position)
        if update is None:
            await self._exit()
            return
        self._last_update = update
        self._enter()
        if not self._playing:
            self._heartbeat_due = now + HEARTBEAT_SECONDS
        await self._publisher.checkpoint(self._virtual.checkpoint(update, hold=not self._playing))

    def _latest_at(self, event: str, position: float) -> dict[str, Any] | None:
        times, payloads = self._series[event]
        if not times:
            return None
        # Before the first recorded value, the first one is the best estimate.
        return payloads[max(bisect_right(times, position) - 1, 0)]

    def _checkpoint_at(self, position: float) -> dict[str, Any] | None:
        """The checkpoint in effect, or None while the player is out of the world."""
        presence_index = bisect_right(self._presence_times, position) - 1
        if presence_index < 0:
            # Before any recorded change, the player is in the world unless it enters later.
            if self._presence and self._presence[0]:
                return None
            return self._latest_at("player:update", position)
        if not self._presence[presence_index]:
            return None
        times = self._series["player:update"][0]
        update_index = bisect_right(times, position) - 1
        if update_index >= 0 and times[update_index] < self._presence_times[presence_index]:
            # The latest checkpoint predates this entry; wait for the next one.
            return None
        return self._latest_at("player:update", position)

    def _enter(self) -> None:
        if not self._in_world:
            self._in_world = True
            self._on_change()

    async def _exit(self) -> None:
        # The next checkpoint places the player in the world again.
        self._last_update = None
        if self._in_world:
            self._in_world = False
            self._on_change()
            await self._publisher.exit()

    async def _emit_message(self, message: RecordedMessage) -> None:
        match message.event:
            case "player:exit":
                await self._exit()
            case "player:update":
                self._last_update = message.payload
                self._enter()
                await self._publisher.checkpoint(self._virtual.checkpoint(message.payload))
            case "player:appearance":
                if payload := self._virtual.appearance(message.payload):
                    await self._publisher.appearance(payload)
            case "player:equipment":
                if payload := self._virtual.equipment(message.payload):
                    await self._publisher.equipment(payload)
            case "animation:action":
                await self._publisher.action(self._virtual.action(message.payload))
            case "mount:transition":
                await self._publisher.mount_transition(self._virtual.mount_transition(message.payload))

    async def _emit_hold(self) -> None:
        if self._last_update is not None:
            await self._publisher.checkpoint(self._virtual.checkpoint(self._last_update, hold=True))

    def _reach_end(self, now: float) -> None:
        if self._loop and self._duration_ms > 0:
            self._anchor_ms = 0.0
            self._anchor_time = now
            self._snapshot_pending = True
        else:
            self._anchor_ms = float(self._duration_ms)
            self._playing = False
            self._ended = True
            self._heartbeat_due = now
        self._touch()

    # -- helpers -----------------------------------------------------------

    def _position(self, now: float) -> float:
        if not self._playing:
            return self._anchor_ms
        elapsed_ms = (now - self._anchor_time) * 1000.0 * self._rate
        return min(float(self._duration_ms), self._anchor_ms + elapsed_ms)

    def _touch(self) -> None:
        self._revision += 1
        self._generation += 1
        self._wake.set()
        self._on_change()

    @staticmethod
    def _validate_rate(rate: float) -> None:
        if not MINIMUM_RATE <= rate <= MAXIMUM_RATE:
            raise PlaybackError(f"rate must be between {MINIMUM_RATE} and {MAXIMUM_RATE}")
