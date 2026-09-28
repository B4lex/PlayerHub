import unittest
from unittest.mock import AsyncMock, call, patch

import server.app as server_app


def valid_state(sequence: int = 10) -> dict[str, bool | int | float | str]:
    return {
        "x": 1.0,
        "y": 2.0,
        "z": 3.0,
        "heading": 0.0,
        "pitch": 0.0,
        "speed": 125.0,
        "direction": 0.0,
        "movementState": "walking",
        "jumping": False,
        "weaponDrawn": True,
        "cellId": 1,
        "worldspaceId": 60,
        "furnitureRefId": 0,
        "graphEpoch": 2,
        "sequence": sequence,
    }


def valid_action(sequence: int = 1) -> dict[str, object]:
    return {
        "actionSequence": sequence,
        "graphEpoch": 2,
        "checkpointSequence": 10,
        "family": "attack",
        "action": "normal",
        "power": False,
        "prerequisites": {"weaponDrawn": True},
    }


class AnimationActionRelayTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self) -> None:
        server_app.player_states.clear()
        server_app.player_action_sequences.clear()
        server_app.checkpoints._pending.clear()

    def tearDown(self) -> None:
        self.setUp()

    async def test_relays_ordered_attack_against_checkpoint(self) -> None:
        emit = AsyncMock()
        server_app.player_states["player-1"] = valid_state()

        with patch.object(server_app.sio, "emit", new=emit):
            result = await server_app.animation_action("player-1", valid_action())

        self.assertEqual(result, {"ok": True})
        emit.assert_awaited_once_with(
            "player:action",
            {"playerId": "player-1", **valid_action()},
            skip_sid="player-1",
        )

    async def test_sends_the_unsent_checkpoint_before_the_action_naming_it(self) -> None:
        emit = AsyncMock()
        server_app.registry.enter("player-1")
        self.addCleanup(server_app.registry.in_world.discard, "player-1")
        server_app.checkpoints.publish("player-2", {"playerId": "player-2"})

        with patch.object(server_app.sio, "emit", new=emit):
            await server_app.player_update("player-1", valid_state())
            result = await server_app.animation_action("player-1", valid_action())

        self.assertEqual(result, {"ok": True})
        checkpoint = {"playerId": "player-1", **server_app.player_states["player-1"]}
        self.assertEqual(
            emit.await_args_list,
            [
                call("player:states", {"states": [checkpoint]}),
                call("player:action", {"playerId": "player-1", **valid_action()}, skip_sid="player-1"),
            ],
        )
        self.assertEqual(server_app.checkpoints._pending, {"player-2": {"playerId": "player-2"}})

    async def test_relays_off_hand_attacks_and_bash_in_order(self) -> None:
        emit = AsyncMock()
        server_app.player_states["player-1"] = valid_state()
        actions = []
        for sequence, kind in enumerate(("left", "dual", "bash"), start=1):
            action = valid_action(sequence)
            action["action"] = kind
            actions.append(action)

        with patch.object(server_app.sio, "emit", new=emit):
            for action in actions:
                result = await server_app.animation_action("player-1", action)
                self.assertEqual(result, {"ok": True})

        self.assertEqual(
            [call.args[1]["action"] for call in emit.await_args_list],
            ["left", "dual", "bash"],
        )

    async def test_relays_power_attack_with_direction(self) -> None:
        emit = AsyncMock()
        server_app.player_states["player-1"] = valid_state()
        action = {**valid_action(), "power": True, "direction": "backward"}

        with patch.object(server_app.sio, "emit", new=emit):
            result = await server_app.animation_action("player-1", action)

        self.assertEqual(result, {"ok": True})
        emit.assert_awaited_once_with(
            "player:action",
            {"playerId": "player-1", **action},
            skip_sid="player-1",
        )

    async def test_rejects_sequence_gap(self) -> None:
        server_app.player_states["player-1"] = valid_state()
        server_app.player_action_sequences["player-1"] = 1

        result = await server_app.animation_action("player-1", valid_action(3))

        self.assertEqual(result, {"ok": False, "error": "non-contiguous action sequence"})

    async def test_rejects_action_for_old_epoch(self) -> None:
        server_app.player_states["player-1"] = valid_state()
        action = valid_action()
        action["graphEpoch"] = 1

        result = await server_app.animation_action("player-1", action)

        self.assertEqual(result, {"ok": False, "error": "incompatible animation checkpoint"})

    async def test_rejects_attack_while_checkpoint_uses_furniture(self) -> None:
        checkpoint = valid_state()
        checkpoint["furnitureRefId"] = 0xA729
        server_app.player_states["player-1"] = checkpoint

        result = await server_app.animation_action("player-1", valid_action())

        self.assertEqual(result, {"ok": False, "error": "incompatible animation checkpoint"})
