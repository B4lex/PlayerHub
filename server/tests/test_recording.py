import json
import tempfile
import unittest
from pathlib import Path

from server.recording import append_animation_action, append_player_update, recording_path
from server.replay import load_recording


def valid_payload(sequence: int) -> dict[str, bool | int | float | str]:
    return {
        "x": 1.0,
        "y": 2.0,
        "z": 3.0,
        "heading": 0.0,
        "speed": 400.0,
        "direction": 0.0,
        "movementState": "running",
        "jumping": False,
        "weaponDrawn": False,
        "cellId": 1,
        "worldspaceId": 60,
        "furnitureRefId": 0,
        "graphEpoch": 1,
        "sequence": sequence,
    }


def valid_action(sequence: int = 1) -> dict[str, object]:
    return {
        "actionSequence": sequence,
        "graphEpoch": 1,
        "checkpointSequence": 1,
        "family": "attack",
        "action": "normal",
        "prerequisites": {"weaponDrawn": True},
    }


class PlayerRecordingTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary_directory.cleanup)
        self.recordings_directory = Path(self.temporary_directory.name) / "recordings"

    def test_persists_a_replayable_recording_per_player(self) -> None:
        recordings = {}
        timestamps = iter((1_000_000_000, 1_075_000_000))

        path = append_player_update(
            recordings,
            self.recordings_directory,
            "player-1",
            valid_payload(1),
            lambda: next(timestamps),
        )
        append_player_update(
            recordings,
            self.recordings_directory,
            "player-1",
            valid_payload(2),
            lambda: next(timestamps),
        )

        document = json.loads(path.read_text(encoding="utf-8"))
        updates = load_recording(path)

        self.assertEqual(path, self.recordings_directory / "player-1.json")
        self.assertEqual(document["playerId"], "player-1")
        self.assertEqual([message["timestampMs"] for message in document["messages"]], [0, 75])
        self.assertEqual(
            [message["event"] for message in document["messages"]],
            ["player:update", "player:update"],
        )
        self.assertEqual([update.delay_seconds for update in updates], [0.0, 0.075])
        self.assertEqual(updates[1].payload["sequence"], 2)

    def test_persists_actions_in_the_same_timeline(self) -> None:
        recordings = {}
        timestamps = iter((1_000_000_000, 1_025_000_000))
        checkpoint = valid_payload(1)
        checkpoint["weaponDrawn"] = True

        path = append_player_update(
            recordings,
            self.recordings_directory,
            "player-1",
            checkpoint,
            lambda: next(timestamps),
        )
        append_animation_action(
            recordings,
            self.recordings_directory,
            "player-1",
            valid_action(),
            lambda: next(timestamps),
        )

        document = json.loads(path.read_text(encoding="utf-8"))
        messages = load_recording(path)

        self.assertEqual(
            [message["event"] for message in document["messages"]],
            ["player:update", "animation:action"],
        )
        self.assertEqual([message.event for message in messages], ["player:update", "animation:action"])
        self.assertEqual(messages[1].delay_seconds, 0.025)
        self.assertEqual(messages[1].payload, valid_action())

    def test_encodes_player_ids_that_cannot_be_filenames(self) -> None:
        path = recording_path(self.recordings_directory, "player/a:b")

        self.assertEqual(path.parent, self.recordings_directory)
        self.assertEqual(path.name, "player%2Fa%3Ab.json")
