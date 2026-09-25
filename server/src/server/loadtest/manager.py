"""Registry of running load-test batches."""

import asyncio
import logging
import secrets
import time
from collections.abc import Awaitable, Callable

from server.loadtest.batch import BatchSettings, LoadBatch, Spawn, Transport
from server.loadtest.client import ClientPublisher
from server.playback.publisher import Publisher
from server.playback.session import PlaybackError, require_checkpoints
from server.recording.store import RecordingStore

logger = logging.getLogger("uvicorn.error")

ClientFactory = Callable[[str], Awaitable[ClientPublisher]]


class BatchNotFound(LookupError):
    pass


class BatchManager:
    """Starts, finds, and stops batches. Batches are runtime state only."""

    def __init__(
        self,
        store: RecordingStore,
        publisher_factory: Callable[[str], Publisher],
        client_factory: ClientFactory = ClientPublisher.connect,
        on_change: Callable[[], None] = lambda: None,
        clock: Callable[[], float] = time.monotonic,
    ) -> None:
        self._store = store
        self._publisher_factory = publisher_factory
        self._client_factory = client_factory
        self._on_change = on_change
        self._clock = clock
        self._batches: dict[str, LoadBatch] = {}

    async def create(
        self, recording_id: str, settings: BatchSettings, *, server_url: str | None
    ) -> LoadBatch:
        """Start a batch. Socket bots connect to ``server_url``, this server's own address."""
        if settings.transport is Transport.SOCKET and server_url is None:
            raise PlaybackError("the server's own address is unknown, so socket bots cannot connect")
        # Parsing a long recording takes long enough to stall live sync.
        recording = await asyncio.to_thread(self._store.load, recording_id)
        require_checkpoints(recording)
        batch_id = secrets.token_hex(4)
        batch = LoadBatch(
            batch_id,
            recording,
            settings,
            self._spawner(batch_id, settings.transport, server_url),
            clock=self._clock,
            on_change=self._on_change,
        )
        self._batches[batch_id] = batch
        batch.start()
        logger.info(
            "Load batch started: id=%s recording=%s players=%d transport=%s seed=%d",
            batch_id,
            recording_id,
            settings.players,
            settings.transport,
            settings.seed,
        )
        self._on_change()
        return batch

    def get(self, batch_id: str) -> LoadBatch:
        batch = self._batches.get(batch_id)
        if batch is None:
            raise BatchNotFound(batch_id)
        return batch

    def batches(self) -> list[LoadBatch]:
        return list(self._batches.values())

    def player_ids(self) -> set[str]:
        return {player_id for batch in self._batches.values() for player_id in batch.player_ids()}

    async def close(self, batch_id: str) -> None:
        batch = self._batches.pop(batch_id, None)
        if batch is None:
            raise BatchNotFound(batch_id)
        await batch.close()
        logger.info("Load batch stopped: id=%s", batch_id)
        self._on_change()

    async def close_all(self) -> None:
        for batch_id in list(self._batches):
            await self.close(batch_id)

    def _spawner(self, batch_id: str, transport: Transport, server_url: str | None) -> Spawn:
        async def spawn_virtual(index: int) -> tuple[str, Publisher]:
            player_id = f"bot:{batch_id}-{index + 1}"
            return player_id, self._publisher_factory(player_id)

        async def spawn_client(index: int) -> tuple[str, Publisher]:
            del index
            assert server_url is not None
            publisher = await self._client_factory(server_url)
            return publisher.player_id, publisher

        return spawn_client if transport is Transport.SOCKET else spawn_virtual
