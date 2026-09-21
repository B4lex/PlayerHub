"""Admin-controlled capture of individual players into recordings."""

import logging
import time
from collections.abc import Callable, Iterable
from dataclasses import dataclass
from datetime import UTC, datetime
from typing import Any

from server.recording.format import (
    RecordedMessage,
    RecordingEnd,
    RecordingHeader,
    RecordingInfo,
)
from server.recording.store import RecordingNotFound, RecordingStore, RecordingWriter

logger = logging.getLogger("uvicorn.error")

MAXIMUM_LABEL_LENGTH = 100


class AlreadyCapturing(RuntimeError):
    def __init__(self, player_id: str, recording_id: str) -> None:
        super().__init__(f"player {player_id} is already being recorded as {recording_id}")
        self.recording_id = recording_id


@dataclass
class _ActiveCapture:
    header: RecordingHeader
    writer: RecordingWriter
    started_ns: int
    message_count: int = 0


class CaptureManager:
    """Records accepted messages of chosen players until they are told to stop.

    A capture also ends by itself when the player disconnects, the server shuts
    down, or the disk fails, so a recording file is always finished with a reason.
    Sync never depends on a capture: a write failure ends that capture and the
    message is relayed regardless.
    """

    def __init__(
        self,
        store: RecordingStore,
        on_change: Callable[[], None] = lambda: None,
        monotonic_ns: Callable[[], int] = time.monotonic_ns,
        now: Callable[[], datetime] = lambda: datetime.now(UTC),
    ) -> None:
        self._store = store
        self._on_change = on_change
        self._monotonic_ns = monotonic_ns
        self._now = now
        self._by_player: dict[str, _ActiveCapture] = {}

    def start(
        self,
        player_id: str,
        player_name: str,
        label: str,
        initial_events: Iterable[tuple[str, dict[str, Any]]],
    ) -> RecordingInfo:
        """Begin recording a player.

        ``initial_events`` is the player's current state (appearance, equipment,
        latest checkpoint), written at time zero so the recording is replayable
        on its own even though it starts mid-session.
        """
        existing = self._by_player.get(player_id)
        if existing is not None:
            raise AlreadyCapturing(player_id, existing.header.id)

        started_at = self._now()
        label = label.strip()[:MAXIMUM_LABEL_LENGTH] or (
            f"{player_name or player_id} {started_at.astimezone():%Y-%m-%d %H:%M}"
        )
        header = RecordingHeader(
            id=self._store.new_id(started_at),
            label=label,
            player_id=player_id,
            player_name=player_name,
            started_at=started_at.isoformat(timespec="seconds").replace("+00:00", "Z"),
        )
        capture = _ActiveCapture(header, self._store.create(header), self._monotonic_ns())
        try:
            for event, payload in initial_events:
                capture.writer.write(RecordedMessage(0, event, payload))
                capture.message_count += 1
        except OSError:
            capture.writer.close(None)
            raise
        self._by_player[player_id] = capture
        logger.info("Capture started: player=%s recording=%s", player_id, header.id)
        self._on_change()
        return self._info(capture)

    def is_capturing(self, player_id: str) -> bool:
        return player_id in self._by_player

    def is_active(self, recording_id: str) -> bool:
        return any(capture.header.id == recording_id for capture in self._by_player.values())

    def recording_id_for(self, player_id: str) -> str | None:
        capture = self._by_player.get(player_id)
        return capture.header.id if capture is not None else None

    def record(self, player_id: str, event: str, payload: dict[str, Any]) -> None:
        """Append an accepted message if the player is being captured."""
        capture = self._by_player.get(player_id)
        if capture is None:
            return
        t_ms = (self._monotonic_ns() - capture.started_ns) // 1_000_000
        try:
            capture.writer.write(RecordedMessage(t_ms, event, payload))
        except OSError:
            logger.exception("Capture failed, stopping: recording=%s", capture.header.id)
            del self._by_player[player_id]
            capture.writer.close(None)
            self._on_change()
            return
        capture.message_count += 1
        self._on_change()

    def stop(self, recording_id: str, reason: str = "stopped") -> RecordingInfo:
        for player_id, capture in self._by_player.items():
            if capture.header.id == recording_id:
                return self._finish(player_id, reason)
        raise RecordingNotFound(recording_id)

    def stop_player(self, player_id: str, reason: str) -> None:
        if player_id in self._by_player:
            self._finish(player_id, reason)

    def stop_all(self, reason: str) -> None:
        for player_id in list(self._by_player):
            self._finish(player_id, reason)

    def active_infos(self) -> list[RecordingInfo]:
        return [self._info(capture) for capture in self._by_player.values()]

    def _finish(self, player_id: str, reason: str) -> RecordingInfo:
        capture = self._by_player.pop(player_id)
        try:
            capture.writer.close(RecordingEnd(self._elapsed_ms(capture), reason))
        except OSError:
            logger.exception("Could not finish recording %s", capture.header.id)
        logger.info(
            "Capture stopped: player=%s recording=%s reason=%s messages=%d",
            player_id,
            capture.header.id,
            reason,
            capture.message_count,
        )
        self._on_change()
        return self._store.info(capture.header.id)

    def _elapsed_ms(self, capture: _ActiveCapture) -> int:
        return (self._monotonic_ns() - capture.started_ns) // 1_000_000

    def _info(self, capture: _ActiveCapture) -> RecordingInfo:
        header = capture.header
        return RecordingInfo(
            id=header.id,
            label=header.label,
            player_id=header.player_id,
            player_name=header.player_name,
            started_at=header.started_at,
            duration_ms=self._elapsed_ms(capture),
            message_count=capture.message_count,
            size_bytes=capture.writer.bytes_written,
            status="recording",
            end_reason=None,
        )
