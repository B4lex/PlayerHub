"""Registry of running replays."""

import asyncio
import logging
import secrets
import time
from collections.abc import Callable

from server.playback.publisher import Publisher
from server.playback.session import PlaybackSession
from server.recording.store import RecordingStore

logger = logging.getLogger("uvicorn.error")


class PlaybackNotFound(LookupError):
    pass


class PlaybackManager:
    """Starts, finds, and stops replays. Each replay is a separate virtual player."""

    def __init__(
        self,
        store: RecordingStore,
        publisher_factory: Callable[[str], Publisher],
        on_change: Callable[[], None] = lambda: None,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self._store = store
        self._publisher_factory = publisher_factory
        self._on_change = on_change
        self._clock = clock
        self._sessions: dict[str, PlaybackSession] = {}

    async def open(
        self,
        recording_id: str,
        *,
        autoplay: bool = True,
        rate: float = 1.0,
        loop: bool = False,
        start_ms: int = 0,
    ) -> PlaybackSession:
        # Parsing a long recording takes long enough to stall live sync.
        recording = await asyncio.to_thread(self._store.load, recording_id)
        session_id = secrets.token_hex(4)
        player_id = f"replay:{session_id}"
        session = PlaybackSession(
            session_id,
            player_id,
            recording,
            self._publisher_factory(player_id),
            autoplay=autoplay,
            rate=rate,
            loop=loop,
            start_ms=start_ms,
            clock=self._clock,
            on_change=self._on_change,
        )
        self._sessions[session_id] = session
        session.start()
        logger.info("Playback started: id=%s recording=%s player=%s", session_id, recording_id, player_id)
        self._on_change()
        return session

    def get(self, session_id: str) -> PlaybackSession:
        session = self._sessions.get(session_id)
        if session is None:
            raise PlaybackNotFound(session_id)
        return session

    def sessions(self) -> list[PlaybackSession]:
        return list(self._sessions.values())

    async def close(self, session_id: str) -> None:
        session = self._sessions.pop(session_id, None)
        if session is None:
            raise PlaybackNotFound(session_id)
        await session.close()
        logger.info("Playback stopped: id=%s", session_id)
        self._on_change()

    async def close_all(self) -> None:
        for session_id in list(self._sessions):
            await self.close(session_id)
