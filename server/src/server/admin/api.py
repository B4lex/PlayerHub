"""REST commands of the admin panel. Live state arrives over the /admin namespace."""

import asyncio

from fastapi import APIRouter, HTTPException, Response

from server.admin.schemas import (
    AdminState,
    MarkerModel,
    OpenPlaybackRequest,
    PlaybackModel,
    PlaybackSettingsRequest,
    RecordingDetail,
    RecordingModel,
    SeekRequest,
    StartRecordingRequest,
)
from server.admin.state import AdminContext, build_state, list_recordings
from server.playback import PlaybackError, PlaybackNotFound, PlaybackSession
from server.recording import (
    AlreadyCapturing,
    RecordingError,
    RecordingInfo,
    RecordingNotFound,
    summarize_markers,
)

# Every route is a coroutine on purpose. Captures and playbacks are event-loop
# state; a plain ``def`` route would run in a worker thread and touch them there.


def create_admin_router(ctx: AdminContext) -> APIRouter:
    router = APIRouter(prefix="/api", tags=["admin"])

    def playback(playback_id: str) -> PlaybackSession:
        try:
            return ctx.playbacks.get(playback_id)
        except PlaybackNotFound:
            raise HTTPException(404, f"no playback {playback_id}") from None

    def recording_info(recording_id: str) -> RecordingInfo:
        for info in ctx.captures.active_infos():
            if info.id == recording_id:
                return info
        try:
            return ctx.store.info(recording_id)
        except RecordingNotFound:
            raise HTTPException(404, f"no recording {recording_id}") from None

    @router.get("/state", response_model=AdminState)
    async def get_state() -> AdminState:
        return build_state(ctx)

    # -- recordings ---------------------------------------------------------

    @router.get("/recordings", response_model=list[RecordingModel])
    async def get_recordings() -> list[RecordingModel]:
        return [RecordingModel.model_validate(info) for info in list_recordings(ctx)]

    @router.get("/recordings/{recording_id}", response_model=RecordingDetail)
    async def get_recording(recording_id: str) -> RecordingDetail:
        info = recording_info(recording_id)
        try:
            recording = await asyncio.to_thread(ctx.store.load, recording_id)
        except RecordingNotFound:
            raise HTTPException(404, f"no recording {recording_id}") from None
        except RecordingError as error:
            raise HTTPException(422, str(error)) from error
        markers = [
            MarkerModel(t_ms=marker.t_ms, kind=marker.kind, label=marker.label)
            for marker in summarize_markers(recording)
        ]
        return RecordingDetail(
            **RecordingModel.model_validate(info).model_dump(), markers=markers
        )

    @router.post("/recordings", status_code=201, response_model=RecordingModel)
    async def start_recording(request: StartRecordingRequest) -> RecordingModel:
        """Start capturing a connected player until told to stop."""
        if request.player_id not in ctx.registry.connected_at:
            raise HTTPException(404, "player is not connected")
        try:
            info = ctx.captures.start(
                request.player_id,
                ctx.registry.name_of(request.player_id),
                request.label,
                ctx.registry.initial_events(request.player_id),
            )
        except AlreadyCapturing as error:
            raise HTTPException(409, str(error)) from error
        except OSError as error:
            raise HTTPException(500, f"cannot write recording: {error}") from error
        return RecordingModel.model_validate(info)

    @router.post("/recordings/{recording_id}/stop", response_model=RecordingModel)
    async def stop_recording(recording_id: str) -> RecordingModel:
        try:
            info = ctx.captures.stop(recording_id)
        except RecordingNotFound:
            raise HTTPException(404, f"recording {recording_id} is not being captured") from None
        return RecordingModel.model_validate(info)

    @router.delete("/recordings/{recording_id}", status_code=204)
    async def delete_recording(recording_id: str) -> Response:
        if ctx.captures.is_active(recording_id):
            raise HTTPException(409, "stop the capture before deleting its recording")
        try:
            ctx.store.delete(recording_id)
        except RecordingNotFound:
            raise HTTPException(404, f"no recording {recording_id}") from None
        # No manager observes a deletion, so announce it here.
        ctx.on_change()
        return Response(status_code=204)

    # -- playbacks ----------------------------------------------------------

    @router.post("/playbacks", status_code=201, response_model=PlaybackModel)
    async def open_playback(request: OpenPlaybackRequest) -> PlaybackModel:
        """Replay a recording as a new virtual player."""
        try:
            session = await ctx.playbacks.open(
                request.recording_id,
                autoplay=request.autoplay,
                rate=request.rate,
                loop=request.loop,
                start_ms=request.start_ms,
            )
        except RecordingNotFound:
            raise HTTPException(404, f"no recording {request.recording_id}") from None
        except (RecordingError, PlaybackError) as error:
            raise HTTPException(422, str(error)) from error
        return PlaybackModel.model_validate(session.state())

    @router.post("/playbacks/{playback_id}/play", response_model=PlaybackModel)
    async def play(playback_id: str) -> PlaybackModel:
        session = playback(playback_id)
        session.play()
        return PlaybackModel.model_validate(session.state())

    @router.post("/playbacks/{playback_id}/pause", response_model=PlaybackModel)
    async def pause(playback_id: str) -> PlaybackModel:
        session = playback(playback_id)
        session.pause()
        return PlaybackModel.model_validate(session.state())

    @router.post("/playbacks/{playback_id}/seek", response_model=PlaybackModel)
    async def seek(playback_id: str, request: SeekRequest) -> PlaybackModel:
        session = playback(playback_id)
        session.seek(request.position_ms)
        return PlaybackModel.model_validate(session.state())

    @router.patch("/playbacks/{playback_id}", response_model=PlaybackModel)
    async def configure(playback_id: str, request: PlaybackSettingsRequest) -> PlaybackModel:
        session = playback(playback_id)
        try:
            session.configure(rate=request.rate, loop=request.loop)
        except PlaybackError as error:
            raise HTTPException(422, str(error)) from error
        return PlaybackModel.model_validate(session.state())

    @router.delete("/playbacks/{playback_id}", status_code=204)
    async def close_playback(playback_id: str) -> Response:
        try:
            await ctx.playbacks.close(playback_id)
        except PlaybackNotFound:
            raise HTTPException(404, f"no playback {playback_id}") from None
        return Response(status_code=204)

    return router
