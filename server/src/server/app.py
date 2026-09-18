import logging
import os
import time
from pathlib import Path
from typing import Any

import socketio
from fastapi import FastAPI

from server.protocol import (
    parse_animation_action,
    parse_player_appearance,
    parse_player_equipment,
    parse_player_update,
    parse_sent_at,
)
from server.recording import (
    PlayerRecording,
    append_animation_action,
    append_player_appearance,
    append_player_equipment,
    append_player_update,
)

logger = logging.getLogger("uvicorn.error")

api = FastAPI(title="Skyrim Multiplayer Server")
# Send the namespace CONNECT before running the connect handler. Otherwise the
# late-joiner replay below reaches the client ahead of CONNECT, and the client
# discards it when the namespace opens.
sio = socketio.AsyncServer(async_mode="asgi", always_connect=True)
player_states: dict[str, dict[str, bool | int | float | str]] = {}
player_action_sequences: dict[str, int] = {}
player_appearances: dict[str, dict[str, Any]] = {}
player_equipment: dict[str, dict[str, Any]] = {}
player_recordings: dict[str, PlayerRecording] = {}
RECORDINGS_DIRECTORY = Path(__file__).resolve().parents[2] / "recordings"
RECORDING_ENABLED = os.getenv("SKYRIM_MP_RECORDING_ENABLED", "false").lower() in {
    "1",
    "true",
    "yes",
    "on",
}


@api.get("/health")
async def health() -> dict[str, str]:
    return {"status": "ok"}


@api.get("/poc")
async def poc() -> dict[str, str]:
    return {
        "message": "Skyrim Multiplayer synchronization server is ready",
        "clientEvent": "player:update",
        "serverEvent": "player:state",
    }


@sio.event
async def connect(sid: str, environ: dict[str, Any]) -> None:
    del environ
    logger.info("Socket.IO client connected: %s", sid)
    # Appearance only changes rarely, so late joiners receive the stored state.
    for player_id, appearance in list(player_appearances.items()):
        if player_id == sid:
            continue
        await sio.emit("player:appearance", {"playerId": player_id, **appearance}, to=sid)
    for player_id, equipment in list(player_equipment.items()):
        if player_id == sid:
            continue
        await sio.emit("player:equipment", {"playerId": player_id, **equipment}, to=sid)


@sio.event
async def disconnect(sid: str, reason: str) -> None:
    player_states.pop(sid, None)
    player_action_sequences.pop(sid, None)
    player_appearances.pop(sid, None)
    player_equipment.pop(sid, None)
    player_recordings.pop(sid, None)
    outbound = {"playerId": sid}
    logger.info("Socket.IO TX player:left payload=%s", outbound)
    await sio.emit("player:left", outbound)
    logger.info("Socket.IO client disconnected: %s (%s)", sid, reason)


@sio.on("player:update")
async def player_update(sid: str, data: Any) -> dict[str, bool | str]:
    logger.debug("Socket.IO RX player:update sid=%s payload=%s", sid, data)
    state = parse_player_update(data)
    if state is None:
        logger.warning("Rejected malformed player:update from %s", sid)
        return {"ok": False, "error": "invalid player transform"}

    previous = player_states.get(sid)
    if previous is not None and state["sequence"] <= previous["sequence"]:
        return {"ok": False, "error": "stale sequence"}
    if previous is not None and state["graphEpoch"] < previous["graphEpoch"]:
        return {"ok": False, "error": "stale graph epoch"}

    if RECORDING_ENABLED:
        try:
            recording_path = append_player_update(
                player_recordings, RECORDINGS_DIRECTORY, sid, state
            )
        except OSError:
            logger.exception("Unable to record player:update from %s", sid)
            return {"ok": False, "error": "unable to record player transform"}
        logger.info("Recorded player:update sid=%s file=%s", sid, recording_path)

    player_states[sid] = state
    outbound: dict[str, bool | int | float | str] = {"playerId": sid, **state}
    # Transport metadata for the receiver's packet delay; not part of the stored
    # or recorded checkpoint, so replays never relay stale emit times.
    sent_at = parse_sent_at(data)
    if sent_at is not None:
        outbound["sentAtMs"] = sent_at
    logger.debug("Socket.IO TX player:state source=%s payload=%s", sid, outbound)
    await sio.emit("player:state", outbound, skip_sid=sid)
    return {"ok": True}


@sio.on("player:appearance")
async def player_appearance(sid: str, data: Any) -> dict[str, bool | str]:
    logger.debug("Socket.IO RX player:appearance sid=%s payload=%s", sid, data)
    appearance = parse_player_appearance(data)
    if appearance is None:
        logger.warning("Rejected malformed player:appearance from %s", sid)
        return {"ok": False, "error": "invalid player appearance"}

    previous = player_appearances.get(sid)
    if previous is not None and appearance["revision"] <= previous["revision"]:
        return {"ok": False, "error": "stale appearance revision"}

    if RECORDING_ENABLED:
        try:
            recording_path = append_player_appearance(
                player_recordings, RECORDINGS_DIRECTORY, sid, appearance
            )
        except OSError:
            logger.exception("Unable to record player:appearance from %s", sid)
            return {"ok": False, "error": "unable to record player appearance"}
        logger.info("Recorded player:appearance sid=%s file=%s", sid, recording_path)

    player_appearances[sid] = appearance
    outbound = {"playerId": sid, **appearance}
    logger.info(
        "Socket.IO TX player:appearance source=%s revision=%s", sid, appearance["revision"]
    )
    await sio.emit("player:appearance", outbound, skip_sid=sid)
    return {"ok": True}


@sio.on("player:equipment")
async def player_equipment_update(sid: str, data: Any) -> dict[str, bool | str]:
    logger.debug("Socket.IO RX player:equipment sid=%s payload=%s", sid, data)
    equipment = parse_player_equipment(data)
    if equipment is None:
        logger.warning("Rejected malformed player:equipment from %s", sid)
        return {"ok": False, "error": "invalid player equipment"}

    previous = player_equipment.get(sid)
    if previous is not None and equipment["revision"] <= previous["revision"]:
        return {"ok": False, "error": "stale equipment revision"}

    if RECORDING_ENABLED:
        try:
            recording_path = append_player_equipment(
                player_recordings, RECORDINGS_DIRECTORY, sid, equipment
            )
        except OSError:
            logger.exception("Unable to record player:equipment from %s", sid)
            return {"ok": False, "error": "unable to record player equipment"}
        logger.info("Recorded player:equipment sid=%s file=%s", sid, recording_path)

    player_equipment[sid] = equipment
    outbound = {"playerId": sid, **equipment}
    logger.info(
        "Socket.IO TX player:equipment source=%s revision=%s", sid, equipment["revision"]
    )
    await sio.emit("player:equipment", outbound, skip_sid=sid)
    return {"ok": True}


@sio.on("animation:action")
async def animation_action(sid: str, data: Any) -> dict[str, bool | str]:
    logger.debug("Socket.IO RX animation:action sid=%s payload=%s", sid, data)
    action = parse_animation_action(data)
    if action is None:
        logger.warning("Rejected malformed animation:action from %s", sid)
        return {"ok": False, "error": "invalid animation action"}

    checkpoint = player_states.get(sid)
    if (
        checkpoint is None
        or action["graphEpoch"] != checkpoint["graphEpoch"]
        or action["checkpointSequence"] > checkpoint["sequence"]
        or not checkpoint["weaponDrawn"]
        or checkpoint["furnitureRefId"] != 0
    ):
        return {"ok": False, "error": "incompatible animation checkpoint"}

    previous_sequence = player_action_sequences.get(sid)
    if previous_sequence is not None and action["actionSequence"] != previous_sequence + 1:
        return {"ok": False, "error": "non-contiguous action sequence"}

    if RECORDING_ENABLED:
        try:
            recording_path = append_animation_action(
                player_recordings, RECORDINGS_DIRECTORY, sid, action
            )
        except OSError:
            logger.exception("Unable to record animation:action from %s", sid)
            return {"ok": False, "error": "unable to record animation action"}
        logger.info("Recorded animation:action sid=%s file=%s", sid, recording_path)

    player_action_sequences[sid] = action["actionSequence"]
    outbound = {"playerId": sid, **action}
    logger.debug("Socket.IO TX player:action source=%s payload=%s", sid, outbound)
    await sio.emit("player:action", outbound, skip_sid=sid)
    return {"ok": True}


@sio.on("latency:probe")
async def latency_probe(sid: str, data: Any = None) -> dict[str, bool | float]:
    # The client times this acknowledgement to measure its round trip, and uses
    # the server clock to stamp payloads that peers compare against their own.
    del sid, data
    return {"ok": True, "serverTimeMs": time.time() * 1000.0}


# Socket.IO handles /socket.io; all other requests are forwarded to FastAPI.
app = socketio.ASGIApp(sio, other_asgi_app=api)
