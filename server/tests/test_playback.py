import asyncio
import unittest
from typing import Any

from recording_fixtures import (
    action,
    appearance,
    checkpoint,
    equipment,
    message,
    recording,
    timeline,
)

from server.playback import PlaybackError, PlaybackSession, PlaybackStatus
from server.playback.virtual_player import VirtualPlayer, hold_pose
from server.protocol import parse_animation_action, parse_player_update


class VirtualPlayerTests(unittest.TestCase):
    def test_numbers_checkpoints_itself_and_produces_valid_payloads(self) -> None:
        virtual = VirtualPlayer()

        first = virtual.checkpoint(checkpoint(sequence=900, graphEpoch=4))
        second = virtual.checkpoint(checkpoint(sequence=3, graphEpoch=4))

        self.assertEqual((first["sequence"], second["sequence"]), (1, 2))
        self.assertEqual((first["graphEpoch"], second["graphEpoch"]), (4, 4))
        self.assertIsNotNone(parse_player_update(second))

    def test_a_break_starts_a_new_epoch_that_never_goes_backwards(self) -> None:
        virtual = VirtualPlayer()
        virtual.checkpoint(checkpoint(1, graphEpoch=5))
        virtual.checkpoint(checkpoint(2, graphEpoch=5))

        virtual.break_continuity()
        after_seek = virtual.checkpoint(checkpoint(1, graphEpoch=1))  # seeked to the start
        following = virtual.checkpoint(checkpoint(2, graphEpoch=1))
        recorded_epoch_change = virtual.checkpoint(checkpoint(3, graphEpoch=2))

        self.assertEqual(after_seek["graphEpoch"], 6)
        self.assertEqual(following["graphEpoch"], 6)
        self.assertEqual(recorded_epoch_change["graphEpoch"], 7)
        self.assertEqual(after_seek["sequence"], 3)

    def test_epoch_never_decreases_even_if_the_recording_does(self) -> None:
        virtual = VirtualPlayer()
        virtual.checkpoint(checkpoint(1, graphEpoch=5))

        self.assertEqual(virtual.checkpoint(checkpoint(2, graphEpoch=2))["graphEpoch"], 5)

    def test_actions_are_contiguous_and_follow_the_latest_checkpoint(self) -> None:
        virtual = VirtualPlayer()
        virtual.checkpoint(checkpoint(10, graphEpoch=3))
        virtual.break_continuity()
        virtual.checkpoint(checkpoint(20, graphEpoch=3))

        first = virtual.action(action(41, 19, graphEpoch=3))
        second = virtual.action(action(42, 19, graphEpoch=3, power=True, direction="left"))

        self.assertEqual([first["actionSequence"], second["actionSequence"]], [1, 2])
        self.assertEqual((first["graphEpoch"], first["checkpointSequence"]), (4, 2))
        self.assertEqual(second["direction"], "left")
        self.assertIsNotNone(parse_animation_action(first))

    def test_an_action_needs_a_checkpoint(self) -> None:
        with self.assertRaises(ValueError):
            VirtualPlayer().action(action())

    def test_resends_appearance_and_equipment_only_when_they_change(self) -> None:
        virtual = VirtualPlayer()

        first = virtual.appearance(appearance(7))
        same = virtual.appearance(appearance(2))  # different revision, same content
        changed = virtual.appearance(appearance(3, name="Renamed"))
        gear = virtual.equipment(equipment(9))
        same_gear = virtual.equipment(equipment(10))
        new_gear = virtual.equipment(equipment(11, armor_form_id=0x999))

        self.assertEqual(first["revision"], 1)
        self.assertIsNone(same)
        self.assertEqual(changed["revision"], 2)
        self.assertEqual((gear["revision"], new_gear["revision"]), (1, 2))
        self.assertIsNone(same_gear)

    def test_hold_pose_stops_locomotion_but_keeps_everything_else(self) -> None:
        running = checkpoint(1, movementState="sprinting", jumping=True, weaponDrawn=True, blocking=True)
        sneaking = checkpoint(1, movementState="sneakingRunning")

        held = hold_pose(running)

        self.assertEqual((held["movementState"], held["speed"], held["jumping"]), ("idle", 0.0, False))
        self.assertTrue(held["weaponDrawn"] and held["blocking"])
        self.assertEqual((held["x"], held["y"], held["z"], held["heading"]), (1.0, 2.0, 3.0, 0.0))
        self.assertEqual(hold_pose(sneaking)["movementState"], "sneakingIdle")
        self.assertEqual(running["movementState"], "sprinting")  # input untouched
        self.assertIsNotNone(parse_player_update(held))


class FakeClock:
    def __init__(self) -> None:
        self.now = 100.0

    def __call__(self) -> float:
        return self.now

    def advance(self, seconds: float) -> None:
        self.now += seconds


class RecordingPublisher:
    def __init__(self) -> None:
        self.events: list[tuple[str, dict[str, Any]]] = []

    async def checkpoint(self, payload: dict[str, Any]) -> None:
        self.events.append(("checkpoint", payload))

    async def action(self, payload: dict[str, Any]) -> None:
        self.events.append(("action", payload))

    async def appearance(self, payload: dict[str, Any]) -> None:
        self.events.append(("appearance", payload))

    async def equipment(self, payload: dict[str, Any]) -> None:
        self.events.append(("equipment", payload))

    async def left(self) -> None:
        self.events.append(("left", {}))

    def take(self) -> list[tuple[str, dict[str, Any]]]:
        events, self.events = self.events, []
        return events

    def positions(self) -> list[float]:
        return [payload["x"] for kind, payload in self.events if kind == "checkpoint"]


class PlaybackSessionTests(unittest.IsolatedAsyncioTestCase):
    def make_session(self, value=None, **options: Any) -> tuple[PlaybackSession, FakeClock, RecordingPublisher]:
        clock = FakeClock()
        publisher = RecordingPublisher()
        changes: list[int] = []
        self.changes = changes
        session = PlaybackSession(
            "abc",
            "replay:abc",
            value or timeline(),
            publisher,
            clock=clock,
            on_change=lambda: changes.append(1),
            **options,
        )
        return session, clock, publisher

    async def test_spawns_with_appearance_equipment_and_the_first_checkpoint(self) -> None:
        session, _, publisher = self.make_session()

        await session.advance()

        self.assertEqual([kind for kind, _ in publisher.events], ["appearance", "equipment", "checkpoint"])
        self.assertEqual(publisher.events[2][1]["x"], 0.0)
        self.assertEqual(session.status, PlaybackStatus.PLAYING)

    async def test_emits_messages_as_their_time_arrives(self) -> None:
        session, clock, publisher = self.make_session()
        await session.advance()
        publisher.take()

        clock.advance(0.25)
        await session.advance()

        self.assertEqual(publisher.positions(), [10.0, 20.0])
        self.assertAlmostEqual(session.next_delay(), 0.05)

    async def test_actions_keep_their_order_after_their_checkpoint(self) -> None:
        session, clock, publisher = self.make_session()
        await session.advance()
        publisher.take()

        clock.advance(0.6)
        await session.advance()

        # The attack was recorded right after the checkpoint at 500 ms.
        kinds = [kind for kind, _ in publisher.events]
        self.assertEqual(kinds, ["checkpoint"] * 5 + ["action", "checkpoint"])
        checkpoint_sequence = publisher.events[4][1]["sequence"]
        self.assertEqual(publisher.events[5][1]["checkpointSequence"], checkpoint_sequence)
        self.assertEqual(publisher.events[5][1]["actionSequence"], 1)

    async def test_pause_freezes_the_position_and_holds_the_pose(self) -> None:
        session, clock, publisher = self.make_session()
        await session.advance()
        clock.advance(0.2)
        await session.advance()
        publisher.take()

        session.pause()
        clock.advance(5.0)
        await session.advance()

        self.assertEqual(session.status, PlaybackStatus.PAUSED)
        self.assertAlmostEqual(session.position_ms(), 200.0)
        [(kind, held)] = publisher.events
        self.assertEqual(kind, "checkpoint")
        self.assertEqual((held["x"], held["movementState"], held["speed"]), (20.0, "idle", 0.0))

    async def test_a_paused_replay_keeps_heartbeating_with_rising_sequences(self) -> None:
        session, clock, publisher = self.make_session()
        await session.advance()
        session.pause()
        await session.advance()
        publisher.take()

        clock.advance(0.1)
        await session.advance()
        self.assertEqual(publisher.events, [])  # not due yet
        clock.advance(0.2)
        await session.advance()
        await session.advance()  # nothing more within the same interval

        [(_, beat)] = publisher.events
        self.assertEqual(beat["movementState"], "idle")
        self.assertGreater(beat["sequence"], 2)

    async def test_resuming_continues_from_where_it_paused(self) -> None:
        session, clock, publisher = self.make_session()
        await session.advance()
        clock.advance(0.2)
        await session.advance()
        session.pause()
        await session.advance()
        clock.advance(10.0)
        publisher.take()

        session.play()
        clock.advance(0.1)
        await session.advance()

        self.assertEqual(session.status, PlaybackStatus.PLAYING)
        self.assertEqual(publisher.positions(), [30.0])

    async def test_seeking_backwards_restarts_numbering_safely(self) -> None:
        session, clock, publisher = self.make_session()
        await session.advance()
        clock.advance(0.7)
        await session.advance()
        before = publisher.take()
        last_before = [payload for kind, payload in before if kind == "checkpoint"][-1]

        session.seek(200)
        await session.advance()

        events = publisher.take()
        self.assertEqual([kind for kind, _ in events], ["checkpoint"])  # same look, so no appearance
        snapshot = events[0][1]
        self.assertEqual(snapshot["x"], 20.0)
        self.assertGreater(snapshot["sequence"], last_before["sequence"])
        self.assertEqual(snapshot["graphEpoch"], last_before["graphEpoch"] + 1)
        self.assertEqual(snapshot["movementState"], "running")  # still playing

        clock.advance(0.15)
        await session.advance()
        self.assertEqual(publisher.positions(), [30.0])

    async def test_seeking_forward_skips_actions_but_shows_the_state_at_the_target(self) -> None:
        session, _, publisher = self.make_session()
        await session.advance()
        publisher.take()

        session.seek(700)
        await session.advance()

        [(kind, snapshot)] = publisher.events
        self.assertEqual((kind, snapshot["x"], snapshot["weaponDrawn"]), ("checkpoint", 70.0, True))

    async def test_seeking_while_paused_shows_a_held_pose_at_the_target(self) -> None:
        session, _, publisher = self.make_session()
        await session.advance()
        session.pause()
        await session.advance()
        publisher.take()

        session.seek(400)
        await session.advance()

        [(_, snapshot)] = publisher.events
        self.assertEqual((snapshot["x"], snapshot["movementState"]), (40.0, "idle"))
        self.assertEqual(session.position_ms(), 400.0)
        self.assertEqual(session.status, PlaybackStatus.PAUSED)

    async def test_seeking_to_a_new_look_resends_appearance(self) -> None:
        messages = [
            message(0, "player:appearance", appearance(1)),
            message(0, "player:update", checkpoint(1)),
            message(500, "player:appearance", appearance(2, name="Renamed")),
            message(500, "player:update", checkpoint(2, x=50.0)),
        ]
        session, _, publisher = self.make_session(recording(messages, 500))
        await session.advance()
        publisher.take()

        session.seek(500)
        await session.advance()

        self.assertEqual([kind for kind, _ in publisher.events], ["appearance", "checkpoint"])
        self.assertEqual(publisher.events[0][1]["name"], "Renamed")
        self.assertEqual(publisher.events[0][1]["revision"], 2)

    async def test_actions_after_a_seek_stay_contiguous(self) -> None:
        session, clock, publisher = self.make_session()
        await session.advance()
        clock.advance(0.6)
        await session.advance()
        session.seek(0)
        await session.advance()
        publisher.take()

        clock.advance(0.6)
        await session.advance()

        [(_, replayed)] = [event for event in publisher.events if event[0] == "action"]
        self.assertEqual(replayed["actionSequence"], 2)

    async def test_rate_scales_the_timeline_from_the_current_position(self) -> None:
        session, clock, _ = self.make_session()
        await session.advance()
        clock.advance(0.1)
        session.configure(rate=2.0)

        clock.advance(0.1)

        self.assertAlmostEqual(session.position_ms(), 300.0)
        with self.assertRaises(PlaybackError):
            session.configure(rate=0)
        with self.assertRaises(PlaybackError):
            session.configure(rate=float("nan"))

    async def test_ends_holds_the_last_pose_and_restarts_from_the_beginning(self) -> None:
        session, clock, publisher = self.make_session()
        await session.advance()
        clock.advance(2.0)
        await session.advance()

        self.assertEqual(session.status, PlaybackStatus.ENDED)
        self.assertEqual(session.state().position_ms, 1000)
        publisher.take()

        await session.advance()  # hold pose at the end
        self.assertEqual(publisher.positions(), [100.0])

        session.play()
        await session.advance()
        self.assertEqual(session.status, PlaybackStatus.PLAYING)
        self.assertEqual(publisher.positions()[-1], 0.0)

    async def test_loops_back_to_the_start(self) -> None:
        session, clock, publisher = self.make_session(loop=True)
        await session.advance()
        clock.advance(1.0)
        await session.advance()
        publisher.take()

        await session.advance()

        self.assertEqual(session.status, PlaybackStatus.PLAYING)
        self.assertEqual(publisher.positions(), [0.0])
        self.assertEqual(session.position_ms(), 0.0)

    async def test_can_start_paused_at_a_position(self) -> None:
        session, _, publisher = self.make_session(autoplay=False, start_ms=300)

        await session.advance()

        self.assertEqual(session.status, PlaybackStatus.PAUSED)
        self.assertEqual(publisher.positions(), [30.0])

    async def test_state_changes_bump_the_revision_and_notify(self) -> None:
        session, _, _ = self.make_session()
        initial = session.state().revision

        session.pause()
        session.seek(500)
        session.configure(loop=True)

        state = session.state()
        self.assertEqual(state.revision, initial + 3)
        self.assertEqual((state.position_ms, state.loop, state.status), (500, True, PlaybackStatus.PAUSED))
        self.assertEqual(len(self.changes), 3)

    async def test_rejects_recordings_it_cannot_show(self) -> None:
        empty = recording([message(0, "player:appearance", appearance())], 0)

        with self.assertRaisesRegex(PlaybackError, "no player:update"):
            self.make_session(empty)

    async def test_runs_on_its_own_in_real_time_and_removes_the_player_on_close(self) -> None:
        clock_session = PlaybackSession(
            "abc", "replay:abc", timeline(200), (publisher := RecordingPublisher()), rate=4.0
        )

        clock_session.start()
        await asyncio.sleep(0.25)

        self.assertEqual(clock_session.status, PlaybackStatus.ENDED)
        self.assertEqual(publisher.positions()[:3], [0.0, 10.0, 20.0])
        await clock_session.close()
        self.assertEqual(publisher.events[-1][0], "left")

    async def test_a_publisher_failure_ends_the_replay_with_an_error(self) -> None:
        class Failing(RecordingPublisher):
            async def checkpoint(self, payload: dict[str, Any]) -> None:
                raise RuntimeError("socket gone")

        session = PlaybackSession("abc", "replay:abc", timeline(), Failing())
        session.start()
        await asyncio.sleep(0.05)

        state = session.state()
        self.assertEqual((state.status, state.error), (PlaybackStatus.ENDED, "socket gone"))
        await session.close()


if __name__ == "__main__":
    unittest.main()
