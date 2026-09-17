import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import AsyncMock, patch

import server.app as server_app


def valid_payload(sequence: int) -> dict[str, bool | int | float | str]:
    return {
        "x": 1.0,
        "y": 2.0,
        "z": 3.0,
        "heading": 0.0,
        "speed": 125.0,
        "direction": 0.0,
        "movementState": "walking",
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
        "power": False,
        "prerequisites": {"weaponDrawn": True},
    }


class PlayerUpdateRecordingTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addAsyncCleanup(self.temporary_directory.cleanup)
        self.recordings_directory = Path(self.temporary_directory.name) / "recordings"
        self.original_recordings_directory = server_app.RECORDINGS_DIRECTORY
        self.original_recording_enabled = server_app.RECORDING_ENABLED
        server_app.RECORDINGS_DIRECTORY = self.recordings_directory
        server_app.RECORDING_ENABLED = True
        server_app.player_states.clear()
        server_app.player_action_sequences.clear()
        server_app.player_recordings.clear()

    async def asyncTearDown(self) -> None:
        server_app.RECORDINGS_DIRECTORY = self.original_recordings_directory
        server_app.RECORDING_ENABLED = self.original_recording_enabled
        server_app.player_states.clear()
        server_app.player_action_sequences.clear()
        server_app.player_recordings.clear()

    async def test_records_only_accepted_updates(self) -> None:
        with patch.object(server_app.sio, "emit", new=AsyncMock()):
            self.assertEqual(await server_app.player_update("player-1", valid_payload(1)), {"ok": True})
            self.assertEqual(
                await server_app.player_update("player-1", valid_payload(1)),
                {"ok": False, "error": "stale sequence"},
            )

        path = self.recordings_directory / "player-1.json"
        document = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(document["playerId"], "player-1")
        self.assertEqual(len(document["messages"]), 1)
        self.assertEqual(document["messages"][0]["payload"]["sequence"], 1)

    async def test_does_not_create_recordings_when_disabled(self) -> None:
        server_app.RECORDING_ENABLED = False

        with patch.object(server_app.sio, "emit", new=AsyncMock()):
            self.assertEqual(await server_app.player_update("player-1", valid_payload(1)), {"ok": True})

        self.assertFalse(self.recordings_directory.exists())

    async def test_records_accepted_actions_after_their_checkpoint(self) -> None:
        checkpoint = valid_payload(1)
        checkpoint["weaponDrawn"] = True

        with patch.object(server_app.sio, "emit", new=AsyncMock()):
            self.assertEqual(
                await server_app.player_update("player-1", checkpoint),
                {"ok": True},
            )
            self.assertEqual(
                await server_app.animation_action("player-1", valid_action()),
                {"ok": True},
            )
            self.assertEqual(
                await server_app.animation_action("player-1", valid_action(3)),
                {"ok": False, "error": "non-contiguous action sequence"},
            )

        path = self.recordings_directory / "player-1.json"
        document = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(
            [message["event"] for message in document["messages"]],
            ["player:update", "animation:action"],
        )

    async def test_rejects_regressing_graph_epoch(self) -> None:
        first = valid_payload(1)
        first["graphEpoch"] = 2
        second = valid_payload(2)

        with patch.object(server_app.sio, "emit", new=AsyncMock()):
            self.assertEqual(await server_app.player_update("player-1", first), {"ok": True})
            self.assertEqual(
                await server_app.player_update("player-1", second),
                {"ok": False, "error": "stale graph epoch"},
            )
