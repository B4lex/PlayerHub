"""Persistent, replay-compatible player state and action recordings."""

from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable
from urllib.parse import quote

PlayerUpdate = dict[str, bool | int | float | str]
AnimationAction = dict[str, Any]


@dataclass
class PlayerRecording:
    path: Path
    started_at_ns: int
    messages: list[dict[str, Any]] = field(default_factory=list)


def recording_path(recordings_directory: Path, player_id: str) -> Path:
    """Return a safe, reversible filename for a Socket.IO player ID."""
    return recordings_directory / f"{quote(player_id, safe='')}.json"


def _append_message(
    recordings: dict[str, PlayerRecording],
    recordings_directory: Path,
    player_id: str,
    event: str,
    payload: dict[str, Any],
    monotonic_ns: Callable[[], int] = time.monotonic_ns,
) -> Path:
    """Append a validated event and atomically persist its player's recording."""
    now_ns = monotonic_ns()
    recording = recordings.get(player_id)
    if recording is None:
        recording = PlayerRecording(recording_path(recordings_directory, player_id), now_ns)
        recordings[player_id] = recording

    timestamp_ms = (now_ns - recording.started_at_ns) // 1_000_000
    recording.messages.append(
        {"timestampMs": timestamp_ms, "event": event, "payload": payload.copy()}
    )
    document = {"playerId": player_id, "messages": recording.messages}

    recordings_directory.mkdir(parents=True, exist_ok=True)
    temporary_path = recording.path.with_suffix(".json.tmp")
    temporary_path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
    temporary_path.replace(recording.path)
    return recording.path


def append_player_update(
    recordings: dict[str, PlayerRecording],
    recordings_directory: Path,
    player_id: str,
    payload: PlayerUpdate,
    monotonic_ns: Callable[[], int] = time.monotonic_ns,
) -> Path:
    """Append a validated player:update checkpoint."""
    return _append_message(
        recordings,
        recordings_directory,
        player_id,
        "player:update",
        payload,
        monotonic_ns,
    )


def append_animation_action(
    recordings: dict[str, PlayerRecording],
    recordings_directory: Path,
    player_id: str,
    payload: AnimationAction,
    monotonic_ns: Callable[[], int] = time.monotonic_ns,
) -> Path:
    """Append a validated animation:action after its checkpoint."""
    return _append_message(
        recordings,
        recordings_directory,
        player_id,
        "animation:action",
        payload,
        monotonic_ns,
    )
