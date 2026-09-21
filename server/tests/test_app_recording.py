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


class PlayerCaptureThroughHandlersTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addAsyncCleanup(self.temporary_directory.cleanup)
        self.original_directory = server_app.recording_store.directory
        server_app.recording_store.directory = Path(self.temporary_directory.name) / "recordings"
        self.clear()

    async def asyncTearDown(self) -> None:
        server_app.captures.stop_all("test cleanup")
        server_app.recording_store.directory = self.original_directory
        self.clear()

    def clear(self) -> None:
        registry = server_app.registry
        for table in (
            registry.connected_at,
            registry.states,
            registry.action_sequences,
            registry.appearances,
            registry.equipment,
        ):
            table.clear()

    async def test_records_only_accepted_messages_of_a_captured_player(self) -> None:
        checkpoint = valid_payload(1)
        checkpoint["weaponDrawn"] = True
        info = server_app.captures.start("player-1", "", "", [])

        with patch.object(server_app.sio, "emit", new=AsyncMock()):
            self.assertEqual(await server_app.player_update("player-1", checkpoint), {"ok": True})
            self.assertEqual(
                await server_app.player_update("player-1", checkpoint),
                {"ok": False, "error": "stale sequence"},
            )
            self.assertEqual(await server_app.animation_action("player-1", valid_action()), {"ok": True})
            self.assertEqual(
                await server_app.animation_action("player-1", valid_action(3)),
                {"ok": False, "error": "non-contiguous action sequence"},
            )
            await server_app.player_update("player-2", valid_payload(1))  # not captured
        server_app.captures.stop(info.id)

        recording = server_app.recording_store.load(info.id)
        self.assertEqual(
            [message.event for message in recording.messages], ["player:update", "animation:action"]
        )
        self.assertEqual(recording.messages[0].payload["sequence"], 1)

    async def test_records_nothing_until_a_capture_is_started(self) -> None:
        with patch.object(server_app.sio, "emit", new=AsyncMock()):
            self.assertEqual(await server_app.player_update("player-1", valid_payload(1)), {"ok": True})

        self.assertFalse(server_app.recording_store.directory.exists())

    async def test_a_failing_capture_never_blocks_the_relay(self) -> None:
        server_app.captures.start("player-1", "", "", [])
        emit = AsyncMock()

        with (
            patch.object(server_app.sio, "emit", new=emit),
            patch.object(server_app.captures._by_player["player-1"].writer, "write", side_effect=OSError("disk full")),
        ):
            result = await server_app.player_update("player-1", valid_payload(1))

        self.assertEqual(result, {"ok": True})
        emit.assert_awaited_once()
        self.assertFalse(server_app.captures.is_capturing("player-1"))

    async def test_disconnecting_ends_the_capture_with_a_reason(self) -> None:
        info = server_app.captures.start("player-1", "", "", [])

        with patch.object(server_app.sio, "emit", new=AsyncMock()):
            await server_app.disconnect("player-1", "transport close")

        self.assertEqual(server_app.recording_store.info(info.id).end_reason, "disconnected")

    async def test_tracks_connected_players_for_the_admin_panel(self) -> None:
        with patch.object(server_app.sio, "emit", new=AsyncMock()):
            await server_app.connect("player-1", {})
            self.assertIn("player-1", server_app.registry.connected_at)

            await server_app.disconnect("player-1", "transport close")

        self.assertNotIn("player-1", server_app.registry.connected_at)

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


if __name__ == "__main__":
    unittest.main()
