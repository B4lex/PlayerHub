"""On-disk recording format: append-only JSON Lines.

A recording is one file with a header line, one line per captured message, and
an end line written when the capture stops::

    {"type":"header","format":"skyrim-mp-recording","version":2,"id":...,"label":...}
    {"type":"message","t":0,"event":"player:update","payload":{...}}
    {"type":"end","t":75266,"reason":"stopped"}

Appending keeps a long capture cheap and crash-safe: a file without an end line
is an interrupted capture, and a torn final line is ignored. Payloads are stored
exactly as the server validated them; playback re-stamps counters such as
``sequence`` itself, so nothing here depends on the capture's numbering.
"""

import json
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

from server.protocol import (
    parse_animation_action,
    parse_player_appearance,
    parse_player_equipment,
    parse_player_update,
)

FORMAT_NAME = "skyrim-mp-recording"
FORMAT_VERSION = 2

PAYLOAD_PARSERS: dict[str, Callable[[Any], dict[str, Any] | None]] = {
    "player:update": parse_player_update,
    "player:appearance": parse_player_appearance,
    "player:equipment": parse_player_equipment,
    "animation:action": parse_animation_action,
}


class RecordingError(ValueError):
    """A recording file that cannot be read or is not valid."""


@dataclass(frozen=True)
class RecordingHeader:
    id: str
    label: str
    player_id: str
    player_name: str
    started_at: str  # ISO-8601 UTC


@dataclass(frozen=True)
class RecordedMessage:
    t_ms: int
    event: str
    payload: dict[str, Any]


@dataclass(frozen=True)
class RecordingEnd:
    t_ms: int
    reason: str


@dataclass
class Recording:
    header: RecordingHeader
    messages: list[RecordedMessage] = field(default_factory=list)
    end: RecordingEnd | None = None

    @property
    def duration_ms(self) -> int:
        if self.end is not None:
            return self.end.t_ms
        return self.messages[-1].t_ms if self.messages else 0


@dataclass(frozen=True)
class RecordingInfo:
    """Listing metadata for a recording, without its messages."""

    id: str
    label: str
    player_id: str
    player_name: str
    started_at: str
    duration_ms: int
    message_count: int
    size_bytes: int
    status: str  # "recording" | "complete" | "interrupted"
    end_reason: str | None


def _dump(value: dict[str, Any]) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def encode_header(header: RecordingHeader) -> str:
    return _dump(
        {
            "type": "header",
            "format": FORMAT_NAME,
            "version": FORMAT_VERSION,
            "id": header.id,
            "label": header.label,
            "playerId": header.player_id,
            "playerName": header.player_name,
            "startedAt": header.started_at,
        }
    )


def encode_message(message: RecordedMessage) -> str:
    return _dump(
        {"type": "message", "t": message.t_ms, "event": message.event, "payload": message.payload}
    )


def encode_end(end: RecordingEnd) -> str:
    return _dump({"type": "end", "t": end.t_ms, "reason": end.reason})


def _is_timestamp(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool) and value >= 0


def _parse_line(line: bytes) -> dict[str, Any] | None:
    try:
        parsed = json.loads(line)
    except json.JSONDecodeError:
        return None
    return parsed if isinstance(parsed, dict) else None


def _parse_header(line: bytes) -> RecordingHeader:
    try:
        raw = json.loads(line)
    except json.JSONDecodeError as error:
        raise RecordingError(f"invalid header: {error}") from error
    if (
        not isinstance(raw, dict)
        or raw.get("type") != "header"
        or raw.get("format") != FORMAT_NAME
        or raw.get("version") != FORMAT_VERSION
    ):
        raise RecordingError(f"not a {FORMAT_NAME} version {FORMAT_VERSION} file")
    fields = ("id", "label", "playerId", "playerName", "startedAt")
    if not all(isinstance(raw.get(name), str) for name in fields):
        raise RecordingError("header is missing required fields")
    return RecordingHeader(
        id=raw["id"],
        label=raw["label"],
        player_id=raw["playerId"],
        player_name=raw["playerName"],
        started_at=raw["startedAt"],
    )


def read_recording(path: Path) -> Recording:
    """Load and fully validate a recording, tolerating a torn final line."""
    try:
        lines = path.read_bytes().splitlines()
    except OSError as error:
        raise RecordingError(f"cannot read {path.name}: {error}") from error
    lines = [line for line in lines if line.strip()]
    if not lines:
        raise RecordingError(f"{path.name} is empty")

    recording = Recording(header=_parse_header(lines[0]))
    previous_t = 0
    for number, line in enumerate(lines[1:], start=2):
        try:
            raw = json.loads(line)
        except json.JSONDecodeError as error:
            if number == len(lines):
                break  # interrupted mid-write; everything before it is intact
            raise RecordingError(f"line {number}: invalid JSON: {error}") from error
        if recording.end is not None:
            raise RecordingError(f"line {number}: content after the end marker")
        if not isinstance(raw, dict) or not _is_timestamp(raw.get("t")):
            raise RecordingError(f"line {number}: missing or invalid timestamp")
        t_ms = raw["t"]
        if t_ms < previous_t:
            raise RecordingError(f"line {number}: timestamp goes backwards")
        previous_t = t_ms

        kind = raw.get("type")
        if kind == "end":
            reason = raw.get("reason")
            if not isinstance(reason, str):
                raise RecordingError(f"line {number}: invalid end reason")
            recording.end = RecordingEnd(t_ms, reason)
        elif kind == "message":
            event = raw.get("event")
            parser = PAYLOAD_PARSERS.get(event) if isinstance(event, str) else None
            if parser is None:
                raise RecordingError(f"line {number}: unsupported event {event!r}")
            payload = parser(raw.get("payload"))
            if payload is None:
                raise RecordingError(f"line {number}: invalid {event} payload")
            recording.messages.append(RecordedMessage(t_ms, event, payload))
        else:
            raise RecordingError(f"line {number}: unknown line type {kind!r}")
    return recording


def scan_recording_info(path: Path) -> RecordingInfo:
    """Summarize a recording from its header and last lines, without validating payloads.

    Cheap enough to list many recordings; :func:`read_recording` does the real
    validation when one is opened.
    """
    try:
        stat = path.stat()
        lines = [line for line in path.read_bytes().splitlines() if line.strip()]
    except OSError as error:
        raise RecordingError(f"cannot read {path.name}: {error}") from error
    if not lines:
        raise RecordingError(f"{path.name} is empty")
    header = _parse_header(lines[0])

    body = lines[1:]
    last = _parse_line(body[-1]) if body else None
    if body and last is None:
        body.pop()  # torn final line
        last = _parse_line(body[-1]) if body else None
    duration_ms = last["t"] if last is not None and _is_timestamp(last.get("t")) else 0
    end: RecordingEnd | None = None
    if last is not None and last.get("type") == "end":
        reason = last.get("reason")
        end = RecordingEnd(duration_ms, reason if isinstance(reason, str) else "unknown")
        body.pop()

    return RecordingInfo(
        id=header.id,
        label=header.label,
        player_id=header.player_id,
        player_name=header.player_name,
        started_at=header.started_at,
        duration_ms=duration_ms,
        message_count=len(body),
        size_bytes=stat.st_size,
        status="complete" if end is not None else "interrupted",
        end_reason=end.reason if end is not None else None,
    )
