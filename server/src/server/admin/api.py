"""REST commands of the admin panel. Live state arrives over the /admin namespace."""

import asyncio
import secrets

from fastapi import APIRouter, HTTPException, Request, Response

from server.admin.schemas import (
    AdminState,
    BatchModel,
    CreateBatchRequest,
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
from server.loadtest import BatchNotFound, BatchSettings, LoadBatch, Transport
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

    def batch(batch_id: str) -> LoadBatch:
        try:
            return ctx.batches.get(batch_id)
        except BatchNotFound:
            raise HTTPException(404, f"no load batch {batch_id}") from None

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

    # -- load-test batches -------------------------------------------------

    @router.post("/batches", status_code=201, response_model=BatchModel)
    async def create_batch(request: CreateBatchRequest, http: Request) -> BatchModel:
        """Replay a recording with many bots, spread over its timeline."""
        settings = BatchSettings(
            players=request.players,
            transport=Transport(request.transport),
            seed=request.seed if request.seed is not None else secrets.randbelow(2**31),
            jitter=request.jitter,
            rate_spread=request.rate_spread,
            ramp_up_seconds=request.ramp_up_seconds,
        )
        try:
            created = await ctx.batches.create(
                request.recording_id, settings, server_url=own_url(http)
            )
        except RecordingNotFound:
            raise HTTPException(404, f"no recording {request.recording_id}") from None
        except (RecordingError, PlaybackError) as error:
            raise HTTPException(422, str(error)) from error
        return BatchModel.model_validate(created.state())

    @router.post("/batches/{batch_id}/pause", response_model=BatchModel)
    async def pause_batch(batch_id: str) -> BatchModel:
        target = batch(batch_id)
        target.pause()
        return BatchModel.model_validate(target.state())

    @router.post("/batches/{batch_id}/resume", response_model=BatchModel)
    async def resume_batch(batch_id: str) -> BatchModel:
        target = batch(batch_id)
        target.resume()
        return BatchModel.model_validate(target.state())

    @router.delete("/batches/{batch_id}", status_code=204)
    async def close_batch(batch_id: str) -> Response:
        try:
            await ctx.batches.close(batch_id)
        except BatchNotFound:
            raise HTTPException(404, f"no load batch {batch_id}") from None
        return Response(status_code=204)

    return router


def own_url(request: Request) -> str | None:
    """This server's address as the admin request reached it.

    The local end of the request's own connection, not its Host header: behind
    the panel's development proxy the Host names the proxy, not this server.
    """
    server = request.scope.get("server")
    if not server or server[1] is None:
        return None
    host, port = server
    return f"http://[{host}]:{port}" if ":" in host else f"http://{host}:{port}"
