import unittest
from unittest.mock import AsyncMock, patch

import server.app as server_app


def valid_state(sequence: int = 1) -> dict[str, bool | int | float | str]:
    return {
        "x": 1.0,
        "y": 2.0,
        "z": 3.0,
        "heading": 0.0,
        "pitch": 0.0,
        "speed": 0.0,
        "direction": 0.0,
        "movementState": "idle",
        "jumping": False,
        "weaponDrawn": True,
        "cellId": 1,
        "worldspaceId": 60,
        "furnitureRefId": 0,
        "graphEpoch": 1,
        "sequence": sequence,
    }


def valid_action(sequence: int = 1, checkpoint_sequence: int = 1) -> dict[str, object]:
    return {
        "actionSequence": sequence,
        "graphEpoch": 1,
        "checkpointSequence": checkpoint_sequence,
        "family": "attack",
        "action": "normal",
        "power": False,
        "prerequisites": {"weaponDrawn": True},
    }


class WorldPresenceTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self) -> None:
        self.clear()
        self.addCleanup(self.clear)

    @staticmethod
    def clear() -> None:
        server_app.registry.connected_at.clear()
        server_app.registry.in_world.clear()
        server_app.registry.states.clear()
        server_app.registry.action_sequences.clear()
        server_app.registry.appearances.clear()
        server_app.registry.equipment.clear()
        server_app.checkpoints._pending.clear()

    async def test_rejects_checkpoints_before_entering_the_world(self) -> None:
        emit = AsyncMock()

        with patch.object(server_app.sio, "emit", new=emit):
            result = await server_app.player_update("player-1", valid_state())

        self.assertEqual(result, {"ok": False, "error": "not in world"})
        self.assertNotIn("player-1", server_app.player_states)
        emit.assert_not_awaited()

    async def test_entering_is_idempotent_and_not_relayed(self) -> None:
        emit = AsyncMock()

        with patch.object(server_app.sio, "emit", new=emit):
            self.assertEqual(await server_app.player_enter("player-1"), {"ok": True})
            self.assertEqual(await server_app.player_enter("player-1", {}), {"ok": True})
            result = await server_app.player_update("player-1", valid_state())
            await server_app.checkpoints.flush()

        self.assertEqual(result, {"ok": True})
        self.assertEqual([call.args[0] for call in emit.await_args_list], ["player:states"])

    async def test_exiting_relays_and_forgets_world_state_but_keeps_the_look(self) -> None:
        emit = AsyncMock()
        server_app.registry.appearances["player-1"] = {"revision": 1}
        server_app.registry.equipment["player-1"] = {"revision": 1}

        with patch.object(server_app.sio, "emit", new=emit):
            await server_app.player_enter("player-1")
            await server_app.player_update("player-1", valid_state(10))
            await server_app.animation_action("player-1", valid_action(4, 10))
            emit.reset_mock()

            result = await server_app.player_exit("player-1")

        self.assertEqual(result, {"ok": True})
        emit.assert_awaited_once_with("player:exit", {"playerId": "player-1"}, skip_sid="player-1")
        self.assertNotIn("player-1", server_app.checkpoints._pending)
        self.assertNotIn("player-1", server_app.player_states)
        self.assertNotIn("player-1", server_app.player_action_sequences)
        self.assertIn("player-1", server_app.player_appearances)
        self.assertIn("player-1", server_app.player_equipment)

    async def test_refuses_checkpoints_and_actions_while_out_of_the_world(self) -> None:
        with patch.object(server_app.sio, "emit", new=AsyncMock()):
            await server_app.player_enter("player-1")
            await server_app.player_update("player-1", valid_state(10))
            await server_app.player_exit("player-1")

            update = await server_app.player_update("player-1", valid_state(11))
            action = await server_app.animation_action("player-1", valid_action(1, 10))

        self.assertEqual(update, {"ok": False, "error": "not in world"})
        self.assertEqual(action, {"ok": False, "error": "incompatible animation checkpoint"})

    async def test_reentering_starts_a_fresh_checkpoint_and_action_stream(self) -> None:
        with patch.object(server_app.sio, "emit", new=AsyncMock()):
            await server_app.player_enter("player-1")
            await server_app.player_update("player-1", valid_state(10))
            await server_app.animation_action("player-1", valid_action(4, 10))
            await server_app.player_exit("player-1")
            await server_app.player_enter("player-1")

            update = await server_app.player_update("player-1", valid_state(2))
            action = await server_app.animation_action("player-1", valid_action(9, 2))

        self.assertEqual(update, {"ok": True})
        self.assertEqual(action, {"ok": True})

    async def test_exiting_when_not_in_the_world_is_not_relayed(self) -> None:
        emit = AsyncMock()

        with patch.object(server_app.sio, "emit", new=emit):
            result = await server_app.player_exit("player-1")

        self.assertEqual(result, {"ok": True})
        emit.assert_not_awaited()

    async def test_disconnecting_removes_the_player_everywhere(self) -> None:
        emit = AsyncMock()

        with patch.object(server_app.sio, "emit", new=emit):
            await server_app.connect("player-1", {})
            await server_app.player_enter("player-1")
            await server_app.player_update("player-1", valid_state())
            await server_app.disconnect("player-1", "transport close")
            await server_app.checkpoints.flush()

        self.assertEqual(
            [call.args for call in emit.await_args_list],
            [
                ("session:welcome", {"playerId": "player-1"}),
                ("player:disconnect", {"playerId": "player-1"}),
            ],
        )
        self.assertNotIn("player-1", server_app.registry.in_world)

    async def test_records_presence_changes_of_a_captured_player(self) -> None:
        with (
            patch.object(server_app.sio, "emit", new=AsyncMock()),
            patch.object(server_app.captures, "record") as record,
        ):
            await server_app.player_enter("player-1")
            await server_app.player_enter("player-1")
            await server_app.player_exit("player-1")
            await server_app.player_exit("player-1")

        self.assertEqual(
            [call.args for call in record.call_args_list],
            [("player-1", "player:enter", {}), ("player-1", "player:exit", {})],
        )


if __name__ == "__main__":
    unittest.main()
