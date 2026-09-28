import asyncio
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from typing import Any
from unittest.mock import AsyncMock

import httpx
from fastapi import FastAPI
from recording_fixtures import (
    appearance,
    checkpoint,
    equipment,
    timeline,
    write_recording,
)

from server.admin import AdminContext, StateBroadcaster, build_state, create_admin_router
from server.admin.api import own_url
from server.loadtest import BatchManager
from server.playback import PlaybackManager, SocketPublisher
from server.recording import CaptureManager, RecordingStore
from server.registry import PlayerRegistry
from server.relay import CheckpointRelay

RECORDING_ID = "20260921-101500-abc123"


class AdminApiTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        temporary_directory = tempfile.TemporaryDirectory()
        self.addAsyncCleanup(asyncio.to_thread, temporary_directory.cleanup)
        self.directory = Path(temporary_directory.name) / "recordings"

        self.registry = PlayerRegistry()
        self.emit = AsyncMock()
        self.checkpoints = CheckpointRelay(self.emit)
        self.changes = 0

        def on_change() -> None:
            self.changes += 1

        store = RecordingStore(self.directory)
        self.captures = CaptureManager(store, on_change=on_change)
        self.playbacks = PlaybackManager(
            store,
            lambda player_id: SocketPublisher(player_id, self.emit, self.registry, self.checkpoints),
            on_change=on_change,
        )
        self.batches = BatchManager(
            store,
            lambda player_id: SocketPublisher(player_id, self.emit, self.registry, self.checkpoints),
            on_change=on_change,
        )
        self.context = AdminContext(
            registry=self.registry,
            store=store,
            captures=self.captures,
            playbacks=self.playbacks,
            batches=self.batches,
            on_change=on_change,
        )
        app = FastAPI()
        app.include_router(create_admin_router(self.context))
        self.client = httpx.AsyncClient(
            transport=httpx.ASGITransport(app=app), base_url="http://admin"
        )
        self.addAsyncCleanup(self.client.aclose)
        self.addAsyncCleanup(self.playbacks.close_all)
        self.addAsyncCleanup(self.batches.close_all)
        self.addCleanup(self.captures.stop_all, "test cleanup")

    def connect_player(self, player_id: str = "player-1") -> None:
        self.registry.add(player_id)
        self.registry.enter(player_id)
        self.registry.appearances[player_id] = appearance(1, name="Lydia")
        self.registry.equipment[player_id] = equipment(1)
        self.registry.states[player_id] = checkpoint(5)

    def store_recording(self) -> str:
        write_recording(self.directory / f"{RECORDING_ID}.jsonl", timeline())
        return RECORDING_ID

    # -- state --------------------------------------------------------------

    async def test_state_lists_players_recordings_and_playbacks_in_camel_case(self) -> None:
        self.connect_player()
        self.store_recording()

        state = (await self.client.get("/api/state")).json()

        self.assertEqual(state["players"][0]["id"], "player-1")
        self.assertEqual(state["players"][0]["name"], "Lydia")
        self.assertTrue(state["players"][0]["inWorld"])
        self.assertEqual(state["players"][0]["movementState"], "running")
        self.assertEqual(state["players"][0]["position"], {"x": 1.0, "y": 2.0, "z": 3.0})
        self.assertEqual(state["recordings"][0]["durationMs"], 1000)
        self.assertEqual(state["recordings"][0]["status"], "complete")
        self.assertEqual(state["playbacks"], [])
        self.assertIn("startedAtMs", state["server"])

    # -- capture ------------------------------------------------------------

    async def test_captures_a_player_until_stopped(self) -> None:
        self.connect_player()

        started = await self.client.post("/api/recordings", json={"playerId": "player-1", "label": "Fight"})

        self.assertEqual(started.status_code, 201)
        recording = started.json()
        self.assertEqual((recording["label"], recording["status"], recording["playerName"]), ("Fight", "recording", "Lydia"))
        state = build_state(self.context)
        self.assertEqual(state.players[0].recording_id, recording["id"])
        self.assertEqual(state.recordings[0].status, "recording")

        self.captures.record("player-1", "player:update", checkpoint(6))
        stopped = await self.client.post(f"/api/recordings/{recording['id']}/stop")

        self.assertEqual(stopped.status_code, 200)
        self.assertEqual(stopped.json()["status"], "complete")
        loaded = self.context.store.load(recording["id"])
        self.assertEqual(
            [message.event for message in loaded.messages],
            ["player:appearance", "player:equipment", "player:update", "player:update"],
        )

    async def test_a_capture_of_a_player_out_of_the_world_starts_with_its_exit(self) -> None:
        self.connect_player()
        self.registry.exit("player-1")

        started = await self.client.post("/api/recordings", json={"playerId": "player-1"})
        await self.client.post(f"/api/recordings/{started.json()['id']}/stop")

        loaded = self.context.store.load(started.json()["id"])
        self.assertEqual(
            [message.event for message in loaded.messages],
            ["player:exit", "player:appearance", "player:equipment"],
        )
        self.assertFalse(build_state(self.context).players[0].in_world)

    async def test_capture_errors(self) -> None:
        self.connect_player()

        unknown = await self.client.post("/api/recordings", json={"playerId": "nobody"})
        first = await self.client.post("/api/recordings", json={"playerId": "player-1"})
        again = await self.client.post("/api/recordings", json={"playerId": "player-1"})
        stop_unknown = await self.client.post("/api/recordings/20260101-000000-000000/stop")
        too_long = await self.client.post(
            "/api/recordings", json={"playerId": "player-1", "label": "x" * 101}
        )

        self.assertEqual(unknown.status_code, 404)
        self.assertEqual(first.status_code, 201)
        self.assertEqual(again.status_code, 409)
        self.assertEqual(stop_unknown.status_code, 404)
        self.assertEqual(too_long.status_code, 422)

    # -- recordings ---------------------------------------------------------

    async def test_recording_detail_includes_timeline_markers(self) -> None:
        recording_id = self.store_recording()

        detail = (await self.client.get(f"/api/recordings/{recording_id}")).json()

        self.assertEqual(detail["id"], recording_id)
        self.assertEqual(
            [(marker["tMs"], marker["kind"]) for marker in detail["markers"]],
            [(300, "weapon"), (500, "attack")],
        )

    async def test_delete_removes_a_recording_but_not_one_being_captured(self) -> None:
        recording_id = self.store_recording()
        self.connect_player()
        capturing = (await self.client.post("/api/recordings", json={"playerId": "player-1"})).json()

        blocked = await self.client.delete(f"/api/recordings/{capturing['id']}")
        deleted = await self.client.delete(f"/api/recordings/{recording_id}")
        missing = await self.client.get(f"/api/recordings/{recording_id}")
        again = await self.client.delete(f"/api/recordings/{recording_id}")

        self.assertEqual((blocked.status_code, deleted.status_code, missing.status_code, again.status_code), (409, 204, 404, 404))

    async def test_unusable_recording_ids_are_not_found(self) -> None:
        for bad in ("a%20b", "..%2F..%2Fetc%2Fpasswd", ".hidden"):
            with self.subTest(bad):
                self.assertEqual((await self.client.get(f"/api/recordings/{bad}")).status_code, 404)
                self.assertEqual((await self.client.delete(f"/api/recordings/{bad}")).status_code, 404)

    # -- playback -----------------------------------------------------------

    async def test_plays_a_recording_as_a_virtual_player_and_controls_it(self) -> None:
        recording_id = self.store_recording()

        opened = await self.client.post(
            "/api/playbacks", json={"recordingId": recording_id, "autoplay": False, "startMs": 300}
        )

        self.assertEqual(opened.status_code, 201)
        playback = opened.json()
        self.assertEqual((playback["status"], playback["positionMs"], playback["durationMs"]), ("paused", 300, 1000))
        base = f"/api/playbacks/{playback['id']}"

        played = (await self.client.post(f"{base}/play")).json()
        paused = (await self.client.post(f"{base}/pause")).json()
        seeked = (await self.client.post(f"{base}/seek", json={"positionMs": 800})).json()
        configured = (await self.client.patch(base, json={"rate": 2, "loop": True})).json()

        self.assertEqual(played["status"], "playing")
        self.assertEqual(paused["status"], "paused")
        self.assertEqual(seeked["positionMs"], 800)
        self.assertEqual((configured["rate"], configured["loop"]), (2.0, True))
        self.assertGreater(configured["revision"], played["revision"])

        players = build_state(self.context).players
        self.assertEqual([(p.id, p.virtual, p.playback_id) for p in players], [(playback["playerId"], True, playback["id"])])
        await asyncio.sleep(0.05)  # let the run loop spawn the actor
        await self.checkpoints.flush()
        events = [call.args[0] for call in self.emit.await_args_list]
        self.assertIn("player:states", events)
        self.assertIn(playback["playerId"], self.registry.appearances)

        removed = await self.client.delete(base)

        self.assertEqual(removed.status_code, 204)
        self.assertEqual(self.emit.await_args_list[-1].args[0], "player:disconnect")
        self.assertNotIn(playback["playerId"], self.registry.appearances)
        self.assertEqual((await self.client.post(f"{base}/play")).status_code, 404)

    async def test_playback_errors(self) -> None:
        recording_id = self.store_recording()
        (self.directory / "20260101-000000-000000.jsonl").write_text("garbage\n", encoding="utf-8")

        unknown_recording = await self.client.post("/api/playbacks", json={"recordingId": "20260101-000000-ffffff"})
        bad_rate = await self.client.post("/api/playbacks", json={"recordingId": recording_id, "rate": 99})
        unreadable = await self.client.post("/api/playbacks", json={"recordingId": "20260101-000000-000000"})
        unknown_playback = await self.client.post("/api/playbacks/nope/seek", json={"positionMs": 1})
        opened = (await self.client.post("/api/playbacks", json={"recordingId": recording_id})).json()
        negative_seek = await self.client.post(f"/api/playbacks/{opened['id']}/seek", json={"positionMs": -5})
        bad_settings = await self.client.patch(f"/api/playbacks/{opened['id']}", json={"rate": 0})

        self.assertEqual(
            [r.status_code for r in (unknown_recording, bad_rate, unreadable, unknown_playback, negative_seek, bad_settings)],
            [404, 422, 422, 404, 422, 422],  # an unparsable file is 422, not 404
        )

    # -- load-test batches --------------------------------------------------

    async def test_runs_a_batch_of_bots_and_controls_it(self) -> None:
        recording_id = self.store_recording()

        created = await self.client.post(
            "/api/batches", json={"recordingId": recording_id, "players": 4, "jitter": 0}
        )

        self.assertEqual(created.status_code, 201)
        batch = created.json()
        self.assertEqual(batch["settings"]["players"], 4)
        self.assertEqual(batch["settings"]["transport"], "virtual")
        self.assertIsInstance(batch["settings"]["seed"], int)
        self.assertEqual(len(batch["bots"]), 4)
        base = f"/api/batches/{batch['id']}"

        paused = (await self.client.post(f"{base}/pause")).json()
        resumed = (await self.client.post(f"{base}/resume")).json()
        state = build_state(self.context)
        closed = await self.client.delete(base)

        self.assertEqual(paused["status"], "paused")
        self.assertIn(resumed["status"], ("starting", "running"))
        self.assertEqual([listed.id for listed in state.batches], [batch["id"]])
        self.assertEqual(closed.status_code, 204)
        self.assertEqual((await self.client.post(f"{base}/pause")).status_code, 404)
        self.assertEqual(build_state(self.context).batches, [])

    async def test_a_seed_reproduces_the_same_bot_layout(self) -> None:
        recording_id = self.store_recording()
        request = {"recordingId": recording_id, "players": 5, "seed": 1234, "rateSpread": 0.2}

        first = (await self.client.post("/api/batches", json=request)).json()
        second = (await self.client.post("/api/batches", json=request)).json()

        self.assertNotEqual(first["id"], second["id"])
        self.assertEqual(
            [(bot["startMs"], bot["rate"]) for bot in first["bots"]],
            [(bot["startMs"], bot["rate"]) for bot in second["bots"]],
        )

    async def test_socket_bots_are_summarized_by_their_batch_not_listed_as_players(self) -> None:
        self.connect_player()
        self.registry.add("bot-sid")
        self.batches.player_ids = lambda: {"bot-sid"}  # type: ignore[method-assign]

        players = build_state(self.context).players

        self.assertEqual([player.id for player in players], ["player-1"])

    async def test_batch_errors(self) -> None:
        recording_id = self.store_recording()

        unknown_recording = await self.client.post(
            "/api/batches", json={"recordingId": "20260101-000000-ffffff", "players": 2}
        )
        no_players = await self.client.post("/api/batches", json={"recordingId": recording_id, "players": 0})
        too_many = await self.client.post("/api/batches", json={"recordingId": recording_id, "players": 501})
        bad_jitter = await self.client.post(
            "/api/batches", json={"recordingId": recording_id, "players": 2, "jitter": 1.5}
        )
        bad_transport = await self.client.post(
            "/api/batches", json={"recordingId": recording_id, "players": 2, "transport": "carrier-pigeon"}
        )
        unknown_batch = await self.client.delete("/api/batches/nope")

        self.assertEqual(
            [r.status_code for r in (unknown_recording, no_players, too_many, bad_jitter, bad_transport, unknown_batch)],
            [404, 422, 422, 422, 422, 404],
        )

    def test_socket_bots_connect_to_the_address_the_request_arrived_at(self) -> None:
        def request(server: Any) -> Any:
            return SimpleNamespace(scope={"server": server})

        self.assertEqual(own_url(request(("127.0.0.1", 3000))), "http://127.0.0.1:3000")
        self.assertEqual(own_url(request(("::1", 3000))), "http://[::1]:3000")
        self.assertIsNone(own_url(request(None)))
        self.assertIsNone(own_url(request(("/run/server.sock", None))))

    async def test_state_changes_are_announced(self) -> None:
        self.connect_player()
        before = self.changes

        await self.client.post("/api/recordings", json={"playerId": "player-1"})

        self.assertGreater(self.changes, before)


class StateBroadcasterTests(unittest.IsolatedAsyncioTestCase):
    def make_broadcaster(self, interval: float = 0.05):
        self.published: list[Any] = []
        self.counter = 0

        def build() -> int:
            self.counter += 1
            return self.counter

        async def publish(state: Any) -> None:
            self.published.append(state)

        return StateBroadcaster(build, publish, min_interval=interval)

    async def test_sends_nothing_without_listeners(self) -> None:
        broadcaster = self.make_broadcaster()

        broadcaster.changed()
        await asyncio.sleep(0.1)

        self.assertEqual(self.published, [])

    async def test_sends_the_first_change_at_once_then_coalesces_a_burst(self) -> None:
        broadcaster = self.make_broadcaster(interval=0.1)
        broadcaster.listener_added()

        broadcaster.changed()
        await asyncio.sleep(0.02)
        self.assertEqual(self.published, [1])

        for _ in range(50):
            broadcaster.changed()
        await asyncio.sleep(0.02)
        self.assertEqual(self.published, [1])  # held back by the interval
        await asyncio.sleep(0.15)

        self.assertEqual(self.published, [1, 2])  # one trailing send with the latest state

    async def test_stops_sending_after_the_last_listener_leaves(self) -> None:
        broadcaster = self.make_broadcaster()
        broadcaster.listener_added()
        broadcaster.listener_removed()

        broadcaster.changed()
        await asyncio.sleep(0.1)

        self.assertEqual(self.published, [])

    async def test_survives_a_failing_publish(self) -> None:
        async def publish(state: Any) -> None:
            raise RuntimeError("client gone")

        broadcaster = StateBroadcaster(lambda: 1, publish, min_interval=0.01)
        broadcaster.listener_added()

        broadcaster.changed()
        await asyncio.sleep(0.05)
        broadcaster.changed()  # still usable afterwards
        await asyncio.sleep(0.05)


class SocketPublisherTests(unittest.IsolatedAsyncioTestCase):
    async def test_relays_as_a_peer_and_registers_late_join_state(self) -> None:
        emit = AsyncMock()
        registry = PlayerRegistry()
        checkpoints = CheckpointRelay(emit)
        publisher = SocketPublisher("replay:1", emit, registry, checkpoints)

        await publisher.appearance({"revision": 1, "name": "Lydia"})
        await publisher.equipment({"revision": 1, "armor": []})
        await publisher.checkpoint({"sequence": 1})
        await publisher.action({"actionSequence": 1})
        await publisher.checkpoint({"sequence": 2})

        self.assertEqual(registry.appearances["replay:1"], {"revision": 1, "name": "Lydia"})
        self.assertEqual(registry.equipment["replay:1"], {"revision": 1, "armor": []})
        self.assertEqual(
            [call.args for call in emit.await_args_list],
            [
                (
                    "player:appearance",
                    {"playerId": "replay:1", "revision": 1, "name": "Lydia", "nameIndex": 1},
                ),
                ("player:equipment", {"playerId": "replay:1", "revision": 1, "armor": []}),
                ("player:states", {"states": [{"playerId": "replay:1", "sequence": 1}]}),
                ("player:action", {"playerId": "replay:1", "actionSequence": 1}),
            ],
        )

        await publisher.left()
        await checkpoints.flush()

        emit.assert_awaited_with("player:disconnect", {"playerId": "replay:1"})
        self.assertNotIn("replay:1", registry.appearances)
        self.assertNotIn("replay:1", registry.name_indices)
        self.assertNotIn("replay:1", registry.appearances)
        self.assertNotIn("replay:1", registry.equipment)


if __name__ == "__main__":
    unittest.main()
