"""Replay recorded player state and animation actions through Socket.IO."""

from __future__ import annotations

import argparse
import json
import math
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import socketio

from server.protocol import (
    parse_animation_action,
    parse_player_appearance,
    parse_player_equipment,
    parse_player_update,
)


@dataclass(frozen=True)
class RecordedUpdate:
    payload: dict[str, Any]
    delay_seconds: float
    event: str = "player:update"


def _non_negative_number(value: Any, field: str, index: int) -> float:
    if (
        isinstance(value, bool)
        or not isinstance(value, (int, float))
        or not math.isfinite(value)
        or value < 0
    ):
        raise ValueError(f"message {index}: {field} must be a non-negative number")
    return float(value)


def _infer_legacy_speed(
    previous_payload: dict[str, bool | int | float | str] | None,
    payload: dict[str, Any],
    delay_ms: float,
) -> float:
    """Derive speed for recordings created before speed was in the protocol."""
    if previous_payload is None or delay_ms <= 0:
        return 0.0

    current_worldspace = payload.get("worldspaceId")
    previous_worldspace = previous_payload.get("worldspaceId")
    if current_worldspace != 0 or previous_worldspace != 0:
        comparable_local_space = (
            current_worldspace != 0 and current_worldspace == previous_worldspace
        )
    else:
        comparable_local_space = payload.get("cellId") == previous_payload.get("cellId")
    if not comparable_local_space:
        return 0.0

    coordinates: list[float] = []
    for field in ("x", "y", "z"):
        current = payload.get(field)
        previous = previous_payload.get(field)
        if (
            isinstance(current, bool)
            or not isinstance(current, (int, float))
            or not math.isfinite(current)
            or isinstance(previous, bool)
            or not isinstance(previous, (int, float))
            or not math.isfinite(previous)
        ):
            return 0.0
        coordinates.append(float(current) - float(previous))

    distance = math.sqrt(sum(delta * delta for delta in coordinates))
    return distance / (delay_ms / 1000.0)


def load_recording(path: Path) -> list[RecordedUpdate]:
    """Load a recording and normalize timing metadata into replay delays."""
    try:
        contents = json.loads(path.read_text(encoding="utf-8"))
    except OSError as error:
        raise ValueError(f"cannot read {path}: {error}") from error
    except json.JSONDecodeError as error:
        raise ValueError(f"invalid JSON in {path}: {error}") from error

    default_delay_ms = 0.0
    if isinstance(contents, dict):
        messages = contents.get("messages")
        if "defaultDelayMs" in contents:
            default_delay_ms = _non_negative_number(
                contents["defaultDelayMs"], "defaultDelayMs", 0
            )
    else:
        messages = contents

    if not isinstance(messages, list) or not messages:
        raise ValueError("recording must be a non-empty JSON array or an object with a non-empty 'messages' array")

    updates: list[RecordedUpdate] = []
    previous_timestamp_ms: float | None = None
    previous_payload: dict[str, bool | int | float | str] | None = None
    for index, message in enumerate(messages, start=1):
        if not isinstance(message, dict):
            raise ValueError(f"message {index}: expected an object")

        raw_payload = message.get("payload", message)
        if not isinstance(raw_payload, dict):
            raise ValueError(f"message {index}: payload must be an object")

        if "timestampMs" in message:
            timestamp_ms = _non_negative_number(message["timestampMs"], "timestampMs", index)
            delay_ms = 0.0 if previous_timestamp_ms is None else timestamp_ms - previous_timestamp_ms
            if delay_ms < 0:
                raise ValueError(f"message {index}: timestampMs must not go backwards")
            previous_timestamp_ms = timestamp_ms
        elif "delayMs" in message:
            delay_ms = _non_negative_number(message["delayMs"], "delayMs", index)
        else:
            delay_ms = default_delay_ms

        event = message.get("event", "player:update")
        if event == "player:update":
            payload_with_speed = raw_payload.copy()
            if "speed" not in payload_with_speed:
                payload_with_speed["speed"] = _infer_legacy_speed(
                    previous_payload, payload_with_speed, delay_ms
                )
            # Recordings made before direction synchronization moved forward.
            payload_with_speed.setdefault("direction", 0.0)
            # Recordings made before jump synchronization are grounded by definition.
            payload_with_speed.setdefault("jumping", False)
            # Recordings made before combat-stance synchronization are sheathed.
            payload_with_speed.setdefault("weaponDrawn", False)
            # Recordings made before furniture synchronization were unoccupied.
            payload_with_speed.setdefault("furnitureRefId", 0)
            # Recordings made before semantic actions belong to the initial graph.
            payload_with_speed.setdefault("graphEpoch", 1)
            payload = parse_player_update(payload_with_speed)
            if payload is None:
                raise ValueError(f"message {index}: invalid player:update payload")
            previous_payload = payload
        elif event == "animation:action":
            payload = parse_animation_action(raw_payload)
            if payload is None:
                raise ValueError(f"message {index}: invalid animation:action payload")
        elif event == "player:appearance":
            payload = parse_player_appearance(raw_payload)
            if payload is None:
                raise ValueError(f"message {index}: invalid player:appearance payload")
        elif event == "player:equipment":
            payload = parse_player_equipment(raw_payload)
            if payload is None:
                raise ValueError(f"message {index}: invalid player:equipment payload")
        else:
            raise ValueError(f"message {index}: unsupported event {event!r}")

        updates.append(
            RecordedUpdate(
                payload=payload,
                delay_seconds=delay_ms / 1000.0,
                event=event,
            )
        )
    return updates


def replay(
    updates: list[RecordedUpdate],
    url: str,
    speed: float,
    loop: bool,
    loop_delay_seconds: float,
    wait_for_ack: bool,
    ack_timeout: float,
) -> None:
    client = socketio.Client(reconnection=False)
    client.connect(url, transports=["websocket"])
    sent = 0
    try:
        while True:
            for update in updates:
                if update.delay_seconds:
                    time.sleep(update.delay_seconds / speed)
                if wait_for_ack:
                    response = client.call(update.event, update.payload, timeout=ack_timeout)
                    if not isinstance(response, dict) or not response.get("ok"):
                        raise RuntimeError(f"server rejected update {sent + 1}: {response!r}")
                else:
                    client.emit(update.event, update.payload)
                sent += 1
                if update.event == "player:update":
                    identifier = f"sequence={update.payload['sequence']}"
                elif update.event in ("player:appearance", "player:equipment"):
                    identifier = f"revision={update.payload['revision']}"
                else:
                    identifier = f"actionSequence={update.payload['actionSequence']}"
                print(f"sent {sent}: event={update.event} {identifier}", flush=True)
            if not loop:
                break
            if loop_delay_seconds:
                time.sleep(loop_delay_seconds)
    finally:
        client.disconnect()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Replay recorded player state, appearance, equipment, and animation actions to a Skyrim Multiplayer Socket.IO server."
    )
    parser.add_argument("recording", type=Path, help="JSON recording to replay")
    parser.add_argument("--url", default="http://127.0.0.1:3000", help="Socket.IO server URL")
    parser.add_argument("--speed", type=float, default=1.0, help="Timing multiplier (default: 1.0)")
    parser.add_argument("--loop", action="store_true", help="Replay the recording until interrupted")
    parser.add_argument("--loop-delay-ms", type=float, default=0.0, help="Pause between loops")
    parser.add_argument("--no-timing", action="store_true", help="Send messages without recorded delays")
    parser.add_argument("--no-ack", action="store_true", help="Do not wait for the server acknowledgement")
    parser.add_argument("--ack-timeout", type=float, default=5.0, help="Acknowledgement timeout in seconds")
    return parser


def main() -> None:
    args = build_parser().parse_args()
    if not math.isfinite(args.speed) or args.speed <= 0:
        raise SystemExit("--speed must be greater than zero")
    if not math.isfinite(args.loop_delay_ms) or args.loop_delay_ms < 0:
        raise SystemExit("--loop-delay-ms must be non-negative")
    if not math.isfinite(args.ack_timeout) or args.ack_timeout <= 0:
        raise SystemExit("--ack-timeout must be greater than zero")

    try:
        updates = load_recording(args.recording)
        if args.no_timing:
            updates = [
                RecordedUpdate(update.payload, 0.0, update.event) for update in updates
            ]
        replay(
            updates,
            args.url,
            args.speed,
            args.loop,
            args.loop_delay_ms / 1000.0,
            not args.no_ack,
            args.ack_timeout,
        )
    except (RuntimeError, ValueError, socketio.exceptions.ConnectionError) as error:
        raise SystemExit(f"replay failed: {error}") from error


if __name__ == "__main__":
    main()
