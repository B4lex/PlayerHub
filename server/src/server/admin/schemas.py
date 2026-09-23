"""Wire types of the admin API. Field names are camelCase, like the sync protocol."""

from typing import Literal

from pydantic import BaseModel, ConfigDict, Field
from pydantic.alias_generators import to_camel

from server.playback.session import MAXIMUM_RATE, MINIMUM_RATE
from server.recording.capture import MAXIMUM_LABEL_LENGTH


class CamelModel(BaseModel):
    model_config = ConfigDict(
        alias_generator=to_camel,
        populate_by_name=True,
        from_attributes=True,
    )


# -- state ------------------------------------------------------------------


class Position(CamelModel):
    x: float
    y: float
    z: float


class PlayerModel(CamelModel):
    """A connected player, or a replay's virtual player."""

    id: str
    name: str
    virtual: bool
    in_world: bool = Field(description="Placed in the game world, not in the main menu or a loading screen")
    connected_at_ms: float | None
    cell_id: int | None
    worldspace_id: int | None
    position: Position | None
    movement_state: str | None
    recording_id: str | None = Field(description="The recording being captured, if any")
    playback_id: str | None = Field(description="The replay this player belongs to, if virtual")


class RecordingModel(CamelModel):
    id: str
    label: str
    player_id: str
    player_name: str
    started_at: str
    duration_ms: int
    message_count: int
    size_bytes: int
    status: Literal["recording", "complete", "interrupted"]
    end_reason: str | None


class PlaybackModel(CamelModel):
    id: str
    recording_id: str
    label: str
    player_id: str
    player_name: str
    status: Literal["playing", "paused", "ended"]
    position_ms: int = Field(description="Timeline position when this state was produced")
    duration_ms: int
    rate: float
    loop: bool
    revision: int = Field(description="Rises on every change; a higher one supersedes")
    error: str | None


class ServerModel(CamelModel):
    started_at_ms: float
    server_time_ms: float


class AdminState(CamelModel):
    """Everything the admin UI shows, replicated as one snapshot."""

    server: ServerModel
    players: list[PlayerModel]
    recordings: list[RecordingModel]
    playbacks: list[PlaybackModel]


class MarkerModel(CamelModel):
    t_ms: int
    kind: Literal["attack", "appearance", "equipment", "weapon", "furniture", "area"]
    label: str


class RecordingDetail(RecordingModel):
    markers: list[MarkerModel]


# -- requests ---------------------------------------------------------------


class StartRecordingRequest(CamelModel):
    player_id: str
    label: str = Field("", max_length=MAXIMUM_LABEL_LENGTH)


class OpenPlaybackRequest(CamelModel):
    recording_id: str
    autoplay: bool = True
    rate: float = Field(1.0, ge=MINIMUM_RATE, le=MAXIMUM_RATE, allow_inf_nan=False)
    loop: bool = False
    start_ms: int = Field(0, ge=0)


class SeekRequest(CamelModel):
    position_ms: float = Field(ge=0, allow_inf_nan=False)


class PlaybackSettingsRequest(CamelModel):
    rate: float | None = Field(None, ge=MINIMUM_RATE, le=MAXIMUM_RATE, allow_inf_nan=False)
    loop: bool | None = None
