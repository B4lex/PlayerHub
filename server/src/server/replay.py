"""Replay a recording to a server as a real Socket.IO client.

The admin panel replays inside the server. This client instead connects like a
player would, which exercises the full ingestion path and lets several machines
watch one replay. It reads the same recording files the server writes.
"""

import argparse
import math
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import socketio

from server.playback.virtual_player import VirtualPlayer
from server.recording.format import RecordingError, read_recording


@dataclass(frozen=True)
class RecordedUpdate:
    payload: dict[str, Any]
    delay_seconds: float
    event: str = "player:update"


def load_recording(path: Path) -> list[RecordedUpdate]:
    """Load a recording as messages with the delay to wait before sending each."""
    recording = read_recording(path)
    if not recording.messages:
        raise ValueError("recording has no messages")

    updates: list[RecordedUpdate] = []
    previous_t_ms = recording.messages[0].t_ms
    for message in recording.messages:
        updates.append(
            RecordedUpdate(
                payload=message.payload,
                delay_seconds=(message.t_ms - previous_t_ms) / 1000.0,
                event=message.event,
            )
        )
        previous_t_ms = message.t_ms
    return updates


def _renumber(virtual: VirtualPlayer, update: RecordedUpdate) -> dict[str, Any] | None:
    """The payload to send now, numbered as this connection's own stream."""
    match update.event:
        case "player:update":
            return virtual.checkpoint(update.payload)
        case "animation:action":
            return virtual.action(update.payload)
        case "player:appearance":
            return virtual.appearance(update.payload)
        case "player:equipment":
            return virtual.equipment(update.payload)
        case "player:enter" | "player:exit":
            return {}
    raise ValueError(f"unsupported event {update.event!r}")


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
    # Numbering belongs to this connection, not the recording, so loops keep
    # rising instead of being rejected as stale.
    virtual = VirtualPlayer()
    sent = 0
    try:
        while True:
            virtual.break_continuity()
            # A recording starts in the world unless it says otherwise.
            client.call("player:enter", {}, timeout=ack_timeout)
            for update in updates:
                if update.delay_seconds:
                    time.sleep(update.delay_seconds / speed)
                payload = _renumber(virtual, update)
                if payload is None:
                    continue
                if wait_for_ack:
                    response = client.call(update.event, payload, timeout=ack_timeout)
                    if not isinstance(response, dict) or not response.get("ok"):
                        raise RuntimeError(f"server rejected update {sent + 1}: {response!r}")
                else:
                    client.emit(update.event, payload)
                sent += 1
                if update.event == "player:update":
                    identifier = f"sequence={payload['sequence']}"
                elif update.event in ("player:appearance", "player:equipment"):
                    identifier = f"revision={payload['revision']}"
                elif update.event in ("player:enter", "player:exit"):
                    identifier = ""
                else:
                    identifier = f"actionSequence={payload['actionSequence']}"
                print(f"sent {sent}: event={update.event} {identifier}", flush=True)
            if not loop:
                break
            if loop_delay_seconds:
                time.sleep(loop_delay_seconds)
    finally:
        client.disconnect()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Replay a recording to a Skyrim Multiplayer server as a Socket.IO player."
    )
    parser.add_argument("recording", type=Path, help="recording (.jsonl) to replay")
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
