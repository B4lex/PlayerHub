"""Valid protocol payloads and recordings shared by the recording, playback and admin tests."""

from pathlib import Path
from typing import Any

from server.recording.format import (
    RecordedMessage,
    Recording,
    RecordingEnd,
    RecordingHeader,
    encode_end,
    encode_header,
    encode_message,
)


def checkpoint(sequence: int = 1, **overrides: Any) -> dict[str, Any]:
    payload: dict[str, Any] = {
        "x": 1.0,
        "y": 2.0,
        "z": 3.0,
        "heading": 0.0,
        "speed": 400.0,
        "direction": 0.0,
        "movementState": "running",
        "jumping": False,
        "weaponDrawn": False,
        "bowDrawn": False,
        "castingLeft": False,
        "castingRight": False,
        "blocking": False,
        "dead": False,
        "mounted": False,
        "swimming": False,
        "cellId": 1,
        "worldspaceId": 60,
        "furnitureRefId": 0,
        "graphEpoch": 1,
        "sequence": sequence,
    }
    payload.update(overrides)
    return payload


def action(sequence: int = 1, checkpoint_sequence: int = 1, **overrides: Any) -> dict[str, Any]:
    payload: dict[str, Any] = {
        "actionSequence": sequence,
        "graphEpoch": 1,
        "checkpointSequence": checkpoint_sequence,
        "family": "attack",
        "action": "normal",
        "power": False,
        "prerequisites": {"weaponDrawn": True},
    }
    payload.update(overrides)
    return payload


def mount_transition(transition: str = "enter", checkpoint_sequence: int = 1, **overrides: Any) -> dict[str, Any]:
    payload: dict[str, Any] = {
        "transition": transition,
        "graphEpoch": 1,
        "checkpointSequence": checkpoint_sequence,
    }
    payload.update(overrides)
    return payload


def appearance(revision: int = 1, name: str = "Lydia Stormblade") -> dict[str, Any]:
    return {
        "revision": revision,
        "name": name,
        "level": 12,
        "race": {"plugin": "Skyrim.esm", "formId": 0x13746},
        "female": True,
        "weight": 42.5,
        "height": 1.0,
        "headParts": [{"plugin": "Skyrim.esm", "formId": 0x51575, "type": 1}],
        "hairColor": {"r": 59, "g": 42, "b": 30},
        "faceDetails": None,
        "bodyTint": {"r": 222, "g": 188, "b": 170},
        "faceMorphs": [0.0] * 19,
        "faceParts": [0, 1, -1, 2],
        "tints": [],
    }


def equipment(revision: int = 1, armor_form_id: int = 0x12E49) -> dict[str, Any]:
    return {
        "revision": revision,
        "armor": [
            {"plugin": "Skyrim.esm", "formId": armor_form_id, "slotMask": 4}
        ],
        "rightHand": {"plugin": "Skyrim.esm", "formId": 0x12EB7, "kind": "weapon", "weaponType": 1},
        "leftHand": None,
        "ammo": None,
    }


def header(recording_id: str = "20260921-101500-abc123", **overrides: Any) -> RecordingHeader:
    values: dict[str, Any] = {
        "id": recording_id,
        "label": "Test capture",
        "player_id": "player-1",
        "player_name": "Lydia Stormblade",
        "started_at": "2026-09-21T10:15:00Z",
    }
    values.update(overrides)
    return RecordingHeader(**values)


def message(t_ms: int, event: str, payload: dict[str, Any]) -> RecordedMessage:
    return RecordedMessage(t_ms, event, payload)


def recording(messages: list[RecordedMessage], duration_ms: int | None = None) -> Recording:
    end = None if duration_ms is None else RecordingEnd(duration_ms, "stopped")
    return Recording(header=header(), messages=messages, end=end)


def timeline(duration_ms: int = 1000, step_ms: int = 100) -> Recording:
    """A recording with appearance and equipment at 0, a checkpoint every step, and one attack.

    The player runs along x (x == t_ms / 10), and draws a weapon at 300 ms so
    the attack at 500 ms is valid.
    """
    messages = [
        message(0, "player:appearance", appearance()),
        message(0, "player:equipment", equipment()),
    ]
    sequence = 0
    for t_ms in range(0, duration_ms + 1, step_ms):
        sequence += 1
        messages.append(
            message(
                t_ms,
                "player:update",
                checkpoint(sequence, x=t_ms / 10.0, weaponDrawn=t_ms >= 300),
            )
        )
        if t_ms == 500:
            messages.append(message(t_ms, "animation:action", action(1, sequence)))
    return recording(messages, duration_ms)


def write_recording(path: Path, value: Recording) -> Path:
    lines = [encode_header(value.header)]
    lines.extend(encode_message(item) for item in value.messages)
    if value.end is not None:
        lines.append(encode_end(value.end))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return path
