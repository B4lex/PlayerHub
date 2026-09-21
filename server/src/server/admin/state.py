"""The admin context and the snapshot of live server state built from it."""

import time
from collections.abc import Callable
from dataclasses import dataclass, field

from server.admin.schemas import (
    AdminState,
    PlaybackModel,
    PlayerModel,
    Position,
    RecordingModel,
    ServerModel,
)
from server.playback import PlaybackManager
from server.recording import CaptureManager, RecordingInfo, RecordingStore
from server.registry import PlayerRegistry


@dataclass
class AdminContext:
    """The parts of the server the admin API reads and controls."""

    registry: PlayerRegistry
    store: RecordingStore
    captures: CaptureManager
    playbacks: PlaybackManager
    # Tells connected panels that state changed; see StateBroadcaster.changed.
    on_change: Callable[[], None] = lambda: None
    started_at: float = field(default_factory=time.time)


def list_recordings(ctx: AdminContext) -> list[RecordingInfo]:
    """Stored recordings, with captures in progress described by their live state."""
    live = {info.id: info for info in ctx.captures.active_infos()}
    infos = [*ctx.store.infos(skip=live), *live.values()]
    infos.sort(key=lambda info: (info.started_at, info.id), reverse=True)
    return infos


def build_state(ctx: AdminContext) -> AdminState:
    players: list[PlayerModel] = []
    for player_id, connected_at in ctx.registry.connected_at.items():
        state = ctx.registry.states.get(player_id)
        players.append(
            PlayerModel(
                id=player_id,
                name=ctx.registry.name_of(player_id),
                virtual=False,
                connected_at_ms=connected_at * 1000.0,
                cell_id=state["cellId"] if state else None,
                worldspace_id=state["worldspaceId"] if state else None,
                position=Position(x=state["x"], y=state["y"], z=state["z"]) if state else None,
                movement_state=state["movementState"] if state else None,
                recording_id=ctx.captures.recording_id_for(player_id),
                playback_id=None,
            )
        )

    playbacks = [session.state() for session in ctx.playbacks.sessions()]
    for playback in playbacks:
        players.append(
            PlayerModel(
                id=playback.player_id,
                name=playback.player_name,
                virtual=True,
                connected_at_ms=None,
                cell_id=None,
                worldspace_id=None,
                position=None,
                movement_state=None,
                recording_id=None,
                playback_id=playback.id,
            )
        )

    now = time.time() * 1000.0
    return AdminState(
        server=ServerModel(started_at_ms=ctx.started_at * 1000.0, server_time_ms=now),
        players=players,
        recordings=[RecordingModel.model_validate(info) for info in list_recordings(ctx)],
        playbacks=[PlaybackModel.model_validate(playback) for playback in playbacks],
    )
