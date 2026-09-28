import unittest
from unittest.mock import AsyncMock, patch

import server.app as server_app


def valid_state(sequence: int = 1) -> dict[str, bool | int | float | str]:
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
        "bowDrawn": False,
        "castingLeft": False,
        "castingRight": False,
        "blocking": False,
        "dead": False,
        "mounted": False,
        "swimming": False,
        "cellId": 1,
        "worldspaceId": 60,
        "furnitureRefId": 0,
        "graphEpoch": 1,
        "sequence": sequence,
    }


class LatencyProbeTests(unittest.IsolatedAsyncioTestCase):
    async def test_acknowledges_without_relaying(self) -> None:
        emit = AsyncMock()

        with patch.object(server_app.sio, "emit", new=emit):
            result = await server_app.latency_probe("player-1")

        self.assertEqual(result, {"ok": True})
        emit.assert_not_awaited()


class CheckpointRelayTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self) -> None:
        server_app.player_states.clear()
        server_app.checkpoints._pending.clear()
        server_app.registry.enter("player-1")

    def tearDown(self) -> None:
        server_app.player_states.clear()
        server_app.checkpoints._pending.clear()
        server_app.registry.in_world.clear()

    async def test_relays_checkpoint_without_client_metadata(self) -> None:
        emit = AsyncMock()

        with patch.object(server_app.sio, "emit", new=emit):
            result = await server_app.player_update(
                "player-1", {**valid_state(), "sentAtMs": 1_700_000_000_250.5}
            )
            await server_app.checkpoints.flush()

        self.assertEqual(result, {"ok": True})
        emit.assert_awaited_once_with(
            "player:states", {"states": [{"playerId": "player-1", **valid_state()}]}
        )
        self.assertEqual(server_app.player_states["player-1"], valid_state())
