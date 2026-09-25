import logging
from collections.abc import AsyncIterator
from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any

import socketio
from fastapi import FastAPI

from server.admin import (
    AdminContext,
    StateBroadcaster,
    build_state,
    create_admin_router,
    mount_admin_ui,
    publish_state,
    register_admin_namespace,
)
from server.loadtest import BatchManager
from server.playback import PlaybackManager, Publisher, SocketPublisher
from server.protocol import (
    parse_animation_action,
    parse_mount_transition,
    parse_player_appearance,
    parse_player_equipment,
    parse_player_update,
)
from server.recording import CaptureManager, RecordingStore
from server.registry import PlayerRegistry

logger = logging.getLogger("uvicorn.error")

RECORDINGS_DIRECTORY = Path(__file__).resolve().parents[2] / "recordings"

HEARTBEAT_INTERVAL_SECONDS = 5
HEARTBEAT_TIMEOUT_SECONDS = 10

# Send the namespace CONNECT before running the connect handler. Otherwise the
# late-joiner replay below reaches the client ahead of CONNECT, and the client
# discards it when the namespace opens.
sio = socketio.AsyncServer(
    async_mode="asgi",
    always_connect=True,
    ping_interval=HEARTBEAT_INTERVAL_SECONDS,
    ping_timeout=HEARTBEAT_TIMEOUT_SECONDS,
)

registry = PlayerRegistry()
# The handlers and their tests address the registry's tables directly.
player_states = registry.states
player_action_sequences = registry.action_sequences
player_mount_transition_sequences = registry.mount_transition_sequences
player_appearances = registry.appearances
player_equipment = registry.equipment


def _notify_admin() -> None:
    # The broadcaster is built from the managers below, so it is looked up late.
    admin_broadcaster.changed()


# Admin panel: recordings are captured on request, and replays run inside the
# server as synthetic players. Every manager reports changes so open panels
# stay current.
recording_store = RecordingStore(RECORDINGS_DIRECTORY)
captures = CaptureManager(recording_store, on_change=_notify_admin)
playbacks = PlaybackManager(
    recording_store,
    lambda player_id: _replay_publisher(player_id),
    on_change=_notify_admin,
)
# Load-test bots replay a recording like the playbacks above, many at once.
batches = BatchManager(
    recording_store,
    lambda player_id: _replay_publisher(player_id),
    on_change=_notify_admin,
)
admin_context = AdminContext(
    registry=registry,
    store=recording_store,
    captures=captures,
    playbacks=playbacks,
    batches=batches,
    on_change=_notify_admin,
)


def _admin_state() -> dict[str, Any]:
    return build_state(admin_context).model_dump(mode="json", by_alias=True)


admin_broadcaster = StateBroadcaster(
    build=_admin_state,
    publish=lambda state: publish_state(sio, state),
)


def _replay_publisher(player_id: str) -> Publisher:
    # Looked up per call so the current ``sio.emit`` is used, not one captured at import.
    async def emit(event: str, payload: dict[str, Any]) -> None:
        await sio.emit(event, payload)

    return SocketPublisher(player_id, emit, registry)


@asynccontextmanager
async def lifespan(_: FastAPI) -> AsyncIterator[None]:
    yield
    captures.stop_all("shutdown")
    await playbacks.close_all()
    await batches.close_all()


api = FastAPI(title="Skyrim Multiplayer Server", lifespan=lifespan)
api.include_router(create_admin_router(admin_context))
register_admin_namespace(sio, admin_broadcaster, _admin_state)
mount_admin_ui(api)


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
    registry.add(sid)
    _notify_admin()
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
    captures.stop_player(sid, "disconnected")
    registry.remove(sid)
    _notify_admin()
    outbound = {"playerId": sid}
    logger.info("Socket.IO TX player:disconnect payload=%s", outbound)
    await sio.emit("player:disconnect", outbound)
    logger.info("Socket.IO client disconnected: %s (%s)", sid, reason)


@sio.on("player:enter")
async def player_enter(sid: str, data: Any = None) -> dict[str, bool]:
    # Peers learn of the player from its next checkpoint; nothing is relayed.
    del data
    if registry.enter(sid):
        logger.info("Socket.IO player entered the world: %s", sid)
        captures.record(sid, "player:enter", {})
        _notify_admin()
    return {"ok": True}


@sio.on("player:exit")
async def player_exit(sid: str, data: Any = None) -> dict[str, bool]:
    del data
    if not registry.exit(sid):
        return {"ok": True}
    logger.info("Socket.IO player left the world: %s", sid)
    captures.record(sid, "player:exit", {})
    _notify_admin()
    outbound = {"playerId": sid}
    logger.info("Socket.IO TX player:exit payload=%s", outbound)
    await sio.emit("player:exit", outbound, skip_sid=sid)
    return {"ok": True}


@sio.on("player:update")
async def player_update(sid: str, data: Any) -> dict[str, bool | str]:
    logger.debug("Socket.IO RX player:update sid=%s payload=%s", sid, data)
    state = parse_player_update(data)
    if state is None:
        logger.warning("Rejected malformed player:update from %s", sid)
        return {"ok": False, "error": "invalid player transform"}
    if sid not in registry.in_world:
        return {"ok": False, "error": "not in world"}

    previous = player_states.get(sid)
    if previous is not None and state["sequence"] <= previous["sequence"]:
        return {"ok": False, "error": "stale sequence"}
    if previous is not None and state["graphEpoch"] < previous["graphEpoch"]:
        return {"ok": False, "error": "stale graph epoch"}

    player_states[sid] = state
    captures.record(sid, "player:update", state)
    _notify_admin()
    outbound: dict[str, bool | int | float | str] = {"playerId": sid, **state}
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

    player_appearances[sid] = appearance
    captures.record(sid, "player:appearance", appearance)
    _notify_admin()
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

    player_equipment[sid] = equipment
    captures.record(sid, "player:equipment", equipment)
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

    player_action_sequences[sid] = action["actionSequence"]
    captures.record(sid, "animation:action", action)
    outbound = {"playerId": sid, **action}
    logger.debug("Socket.IO TX player:action source=%s payload=%s", sid, outbound)
    await sio.emit("player:action", outbound, skip_sid=sid)
    return {"ok": True}


@sio.on("mount:transition")
async def mount_transition(sid: str, data: Any) -> dict[str, bool | str]:
    logger.debug("Socket.IO RX mount:transition sid=%s payload=%s", sid, data)
    transition = parse_mount_transition(data)
    if transition is None:
        logger.warning("Rejected malformed mount:transition from %s", sid)
        return {"ok": False, "error": "invalid mount transition"}

    # Both transitions start while the player is reported riding: the checkpoint
    # turns mounted when the mount animation starts, and back once the dismount
    # animation has ended.
    checkpoint = player_states.get(sid)
    if (
        checkpoint is None
        or transition["graphEpoch"] != checkpoint["graphEpoch"]
        or transition["checkpointSequence"] > checkpoint["sequence"]
        or not checkpoint["mounted"]
        or checkpoint["dead"]
    ):
        return {"ok": False, "error": "incompatible mount checkpoint"}

    previous_sequence = player_mount_transition_sequences.get(sid)
    if previous_sequence is not None and transition["checkpointSequence"] <= previous_sequence:
        return {"ok": False, "error": "stale mount transition"}

    player_mount_transition_sequences[sid] = transition["checkpointSequence"]
    captures.record(sid, "mount:transition", transition)
    outbound = {"playerId": sid, **transition}
    logger.info("Socket.IO TX player:mount source=%s payload=%s", sid, outbound)
    await sio.emit("player:mount", outbound, skip_sid=sid)
    return {"ok": True}


@sio.on("latency:probe")
async def latency_probe(sid: str, data: Any = None) -> dict[str, bool]:
    # The client times this acknowledgement to measure its round trip, and treats
    # a missing one as a lost connection.
    del sid, data
    return {"ok": True}


# Socket.IO handles /socket.io; all other requests are forwarded to FastAPI.
app = socketio.ASGIApp(sio, other_asgi_app=api)
