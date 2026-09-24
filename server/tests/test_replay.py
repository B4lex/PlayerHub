import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, call, patch

from recording_fixtures import (
    action,
    appearance,
    checkpoint,
    message,
    mount_transition,
    recording,
    write_recording,
)

from server.replay import RecordedUpdate, load_recording, replay


class LoadRecordingTests(unittest.TestCase):
    def write(self, value) -> Path:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        return write_recording(Path(directory.name) / "a.jsonl", value)

    def test_derives_delays_from_timestamps(self) -> None:
        path = self.write(
            recording(
                [
                    message(120, "player:update", checkpoint(1)),
                    message(170, "player:update", checkpoint(2)),
                    message(170, "player:update", checkpoint(3)),
                ]
            )
        )

        updates = load_recording(path)

        self.assertEqual([update.delay_seconds for update in updates], [0.0, 0.05, 0.0])

    def test_loads_interleaved_events(self) -> None:
        path = self.write(
            recording(
                [
                    message(0, "player:appearance", appearance()),
                    message(100, "player:update", checkpoint(1, weaponDrawn=True)),
                    message(125, "animation:action", action()),
                ]
            )
        )

        updates = load_recording(path)

        self.assertEqual(
            [update.event for update in updates],
            ["player:appearance", "player:update", "animation:action"],
        )
        self.assertEqual([update.delay_seconds for update in updates], [0.0, 0.1, 0.025])
        self.assertEqual(updates[2].payload, action())

    def test_rejects_an_invalid_recording(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "a.jsonl"
        path.write_text("not a recording\n", encoding="utf-8")

        with self.assertRaises(ValueError):
            load_recording(path)

    def test_rejects_a_recording_without_messages(self) -> None:
        with self.assertRaisesRegex(ValueError, "no messages"):
            load_recording(self.write(recording([], 0)))


class ReplayTests(unittest.TestCase):
    @patch("server.replay.socketio.Client")
    def test_sends_updates_and_actions_through_their_protocol_events(self, client_factory: Mock) -> None:
        client = client_factory.return_value
        client.call.return_value = {"ok": True}
        drawn = checkpoint(1, weaponDrawn=True)
        messages = [
            RecordedUpdate(drawn, 0.0, "player:update"),
            RecordedUpdate(action(), 0.0, "animation:action"),
        ]

        replay(messages, "http://localhost:3000", 1.0, False, 0.0, True, 5.0)

        self.assertEqual(
            client.call.call_args_list,
            [
                call("player:enter", {}, timeout=5.0),
                call("player:update", drawn, timeout=5.0),
                call("animation:action", action(), timeout=5.0),
            ],
        )
        client.disconnect.assert_called_once_with()

    @patch("server.replay.socketio.Client")
    def test_sends_mount_transitions_against_the_latest_checkpoint(self, client_factory: Mock) -> None:
        client = client_factory.return_value
        client.call.return_value = {"ok": True}
        messages = [
            RecordedUpdate(checkpoint(41, mounted=True), 0.0, "player:update"),
            RecordedUpdate(mount_transition("enter", 41), 0.0, "mount:transition"),
        ]

        replay(messages, "http://localhost:3000", 1.0, False, 0.0, True, 5.0)

        self.assertEqual(
            client.call.call_args_list[-1],
            call("mount:transition", mount_transition("enter", 1), timeout=5.0),
        )

    @patch("server.replay.socketio.Client")
    def test_numbers_messages_itself_so_looping_is_never_stale(self, client_factory: Mock) -> None:
        client = client_factory.return_value
        client.call.return_value = {"ok": True}
        messages = [
            RecordedUpdate(checkpoint(41, graphEpoch=3), 0.0, "player:update"),
            RecordedUpdate(checkpoint(42, graphEpoch=3), 0.0, "player:update"),
        ]

        with patch("server.replay.time.sleep", side_effect=[None, KeyboardInterrupt]):
            with self.assertRaises(KeyboardInterrupt):
                replay(messages, "http://localhost:3000", 1.0, True, 0.5, True, 5.0)

        sent = [call_.args[1] for call_ in client.call.call_args_list if call_.args[0] == "player:update"]
        self.assertEqual([(p["sequence"], p["graphEpoch"]) for p in sent], [(1, 3), (2, 3), (3, 4), (4, 4)])

    @patch("server.replay.socketio.Client")
    def test_stops_when_the_server_rejects_a_message(self, client_factory: Mock) -> None:
        client = client_factory.return_value
        client.call.return_value = {"ok": False, "error": "stale sequence"}

        with self.assertRaisesRegex(RuntimeError, "rejected update 1"):
            replay(
                [RecordedUpdate(checkpoint(1), 0.0)], "http://localhost:3000", 1.0, False, 0.0, True, 5.0
            )
        client.disconnect.assert_called_once_with()


if __name__ == "__main__":
    unittest.main()
