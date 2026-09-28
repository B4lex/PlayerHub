import asyncio
import tempfile
import unittest
from pathlib import Path
from typing import Any

from recording_fixtures import timeline, write_recording
from test_playback import RecordingPublisher

from server.loadtest import (
    BatchManager,
    BatchNotFound,
    BatchSettings,
    BatchStatus,
    BotStatus,
    ClientPublisher,
    Transport,
    plan_bots,
)
from server.loadtest.batch import bot_name
from server.playback import PlaybackError
from server.protocol import MAXIMUM_NAME_BYTES
from server.recording import RecordingStore

RECORDING_ID = "20260921-101500-abc123"


def settings(players: int = 4, **overrides: Any) -> BatchSettings:
    values: dict[str, Any] = {
        "players": players,
        "transport": Transport.VIRTUAL,
        "seed": 7,
        "jitter": 0.5,
        "rate_spread": 0.0,
        "ramp_up_seconds": 0.0,
    }
    values.update(overrides)
    return BatchSettings(**values)


class PlanTests(unittest.TestCase):
    def test_without_jitter_bots_are_spaced_evenly(self) -> None:
        plans = plan_bots(10, 120_000, seed=1, jitter=0.0)

        self.assertEqual([plan.start_ms for plan in plans], list(range(0, 120_000, 12_000)))
        self.assertEqual({plan.rate for plan in plans}, {1.0})
        self.assertEqual({plan.join_delay_seconds for plan in plans}, {0.0})

    def test_jitter_keeps_each_bot_near_its_slot_and_is_reproducible(self) -> None:
        plans = plan_bots(10, 120_000, seed=42, jitter=1.0)

        for plan in plans:
            distance = abs((plan.start_ms - plan.index * 12_000 + 60_000) % 120_000 - 60_000)
            self.assertLessEqual(distance, 6_000)
        self.assertEqual(plans, plan_bots(10, 120_000, seed=42, jitter=1.0))
        self.assertNotEqual(plans, plan_bots(10, 120_000, seed=43, jitter=1.0))

    def test_rate_spread_varies_speed_without_moving_the_offsets(self) -> None:
        steady = plan_bots(20, 60_000, seed=5, rate_spread=0.0)
        varied = plan_bots(20, 60_000, seed=5, rate_spread=0.1)

        self.assertEqual([plan.start_ms for plan in steady], [plan.start_ms for plan in varied])
        self.assertTrue(all(0.9 <= plan.rate <= 1.1 for plan in varied))
        self.assertGreater(len({plan.rate for plan in varied}), 1)

    def test_bots_joining_later_start_further_along_to_keep_their_spacing(self) -> None:
        plans = plan_bots(4, 100_000, seed=1, jitter=0.0, ramp_up_seconds=20.0)

        self.assertEqual([plan.join_delay_seconds for plan in plans], [0.0, 5.0, 10.0, 15.0])
        self.assertEqual([plan.start_ms for plan in plans], [0, 30_000, 60_000, 90_000])

    def test_an_empty_recording_starts_every_bot_at_zero(self) -> None:
        self.assertEqual({plan.start_ms for plan in plan_bots(3, 0, seed=1)}, {0})

    def test_bot_names_are_numbered_and_fit_the_protocol(self) -> None:
        self.assertEqual(bot_name("Lydia", 2), "Lydia #3")
        long_name = bot_name("é" * 100, 499)
        self.assertLessEqual(len(long_name.encode()), MAXIMUM_NAME_BYTES)
        self.assertTrue(long_name.endswith(" #500"))


class FakeSocketClient:
    def __init__(self) -> None:
        self.connected = True
        self.emitted: list[tuple[str, dict[str, Any]]] = []

    def get_sid(self) -> str:
        return "sid-1"

    async def emit(self, event: str, payload: dict[str, Any]) -> None:
        self.emitted.append((event, payload))

    async def disconnect(self) -> None:
        self.connected = False


class ClientPublisherTests(unittest.IsolatedAsyncioTestCase):
    async def test_enters_the_world_before_its_first_checkpoint_and_after_an_exit(self) -> None:
        client = FakeSocketClient()
        publisher = ClientPublisher(client)  # type: ignore[arg-type]

        await publisher.checkpoint({"sequence": 1})
        await publisher.checkpoint({"sequence": 2})
        await publisher.exit()
        await publisher.checkpoint({"sequence": 3})

        self.assertEqual(publisher.player_id, "sid-1")
        self.assertEqual(
            [event for event, _ in client.emitted],
            ["player:enter", "player:update", "player:update", "player:exit", "player:enter", "player:update"],
        )

    async def test_sends_player_events_under_their_client_names(self) -> None:
        client = FakeSocketClient()
        publisher = ClientPublisher(client)  # type: ignore[arg-type]

        await publisher.action({"actionSequence": 1})
        await publisher.appearance({"revision": 1})
        await publisher.equipment({"revision": 1})

        self.assertEqual(
            [event for event, _ in client.emitted],
            ["animation:action", "player:appearance", "player:equipment"],
        )

    async def test_a_lost_connection_fails_the_replay(self) -> None:
        client = FakeSocketClient()
        publisher = ClientPublisher(client)  # type: ignore[arg-type]
        await publisher.left()

        with self.assertRaises(ConnectionError):
            await publisher.checkpoint({"sequence": 1})


class BatchManagerTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        temporary_directory = tempfile.TemporaryDirectory()
        self.addAsyncCleanup(asyncio.to_thread, temporary_directory.cleanup)
        directory = Path(temporary_directory.name)
        write_recording(directory / f"{RECORDING_ID}.jsonl", timeline(duration_ms=4000))
        self.publishers: dict[str, RecordingPublisher] = {}
        self.clients: list[str] = []
        self.changes = 0

        def publisher_factory(player_id: str) -> RecordingPublisher:
            publisher = RecordingPublisher()
            self.publishers[player_id] = publisher
            return publisher

        def on_change() -> None:
            self.changes += 1

        self.manager = BatchManager(
            RecordingStore(directory),
            publisher_factory,  # type: ignore[arg-type]
            client_factory=self.connect_client,
            on_change=on_change,
        )
        self.addAsyncCleanup(self.manager.close_all)

    async def connect_client(self, url: str) -> ClientPublisher:
        self.clients.append(url)
        if len(self.clients) == 2:
            raise ConnectionError("connection refused")
        client = FakeSocketClient()
        client.get_sid = lambda: f"sid-{len(self.clients)}"  # type: ignore[method-assign]
        return ClientPublisher(client)  # type: ignore[arg-type]

    async def settle(self) -> None:
        for _ in range(5):
            await asyncio.sleep(0)

    async def test_bots_replay_from_their_own_offsets_under_their_own_names(self) -> None:
        batch = await self.manager.create(RECORDING_ID, settings(4, jitter=0.0), server_url=None)
        await self.settle()

        state = batch.state()
        self.assertEqual(state.status, BatchStatus.RUNNING)
        self.assertEqual([bot.start_ms for bot in state.bots], [0, 1000, 2000, 3000])
        self.assertEqual({bot.status for bot in state.bots}, {BotStatus.PLAYING})
        player_ids = [f"bot:{batch.id}-{index}" for index in range(1, 5)]
        self.assertEqual([bot.player_id for bot in state.bots], player_ids)
        self.assertEqual(self.manager.player_ids(), set(player_ids))
        for index, player_id in enumerate(player_ids):
            events = self.publishers[player_id].events
            appearance = next(payload for kind, payload in events if kind == "appearance")
            checkpoint = next(payload for kind, payload in events if kind == "checkpoint")
            self.assertEqual(appearance["name"], f"Lydia Stormblade #{index + 1}")
            self.assertEqual(checkpoint["x"], index * 100.0)

    async def test_bots_join_over_the_ramp_up(self) -> None:
        batch = await self.manager.create(
            RECORDING_ID, settings(3, ramp_up_seconds=30.0), server_url=None
        )
        await self.settle()

        state = batch.state()
        self.assertEqual(state.status, BatchStatus.STARTING)
        self.assertEqual(
            [bot.status for bot in state.bots],
            [BotStatus.PLAYING, BotStatus.PENDING, BotStatus.PENDING],
        )

    async def test_pause_holds_every_bot_and_resume_plays_them(self) -> None:
        batch = await self.manager.create(RECORDING_ID, settings(3), server_url=None)
        await self.settle()

        batch.pause()
        paused = batch.state()
        batch.resume()
        resumed = batch.state()

        self.assertEqual(paused.status, BatchStatus.PAUSED)
        self.assertEqual({bot.status for bot in paused.bots}, {BotStatus.PAUSED})
        self.assertEqual(resumed.status, BatchStatus.RUNNING)
        self.assertEqual({bot.status for bot in resumed.bots}, {BotStatus.PLAYING})

    async def test_closing_removes_every_bot(self) -> None:
        batch = await self.manager.create(RECORDING_ID, settings(3), server_url=None)
        await self.settle()

        await self.manager.close(batch.id)

        for publisher in self.publishers.values():
            self.assertEqual(publisher.events[-1][0], "left")
        self.assertEqual(self.manager.batches(), [])
        self.assertEqual(self.manager.player_ids(), set())
        with self.assertRaises(BatchNotFound):
            await self.manager.close(batch.id)

    async def test_closing_during_the_ramp_up_stops_joining(self) -> None:
        batch = await self.manager.create(
            RECORDING_ID, settings(3, ramp_up_seconds=30.0), server_url=None
        )
        await self.settle()

        await self.manager.close(batch.id)
        await self.settle()

        self.assertEqual(len(self.publishers), 1)

    async def test_socket_bots_connect_and_one_that_cannot_is_reported(self) -> None:
        batch = await self.manager.create(
            RECORDING_ID,
            settings(3, transport=Transport.SOCKET),
            server_url="http://127.0.0.1:3000",
        )
        await self.settle()

        state = batch.state()
        self.assertEqual(self.clients, ["http://127.0.0.1:3000"] * 3)
        self.assertEqual([bot.player_id for bot in state.bots], ["sid-1", None, "sid-3"])
        self.assertEqual(
            [bot.status for bot in state.bots],
            [BotStatus.PLAYING, BotStatus.FAILED, BotStatus.PLAYING],
        )
        self.assertEqual(state.bots[1].error, "connection refused")
        self.assertEqual(state.status, BatchStatus.RUNNING)

    async def test_socket_bots_need_the_server_address(self) -> None:
        with self.assertRaises(PlaybackError):
            await self.manager.create(
                RECORDING_ID, settings(transport=Transport.SOCKET), server_url=None
            )
        self.assertEqual(self.manager.batches(), [])


if __name__ == "__main__":
    unittest.main()
