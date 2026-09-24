import math
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
    async def test_acknowledges_with_server_time_without_relaying(self) -> None:
        emit = AsyncMock()

        with (
            patch.object(server_app.sio, "emit", new=emit),
            patch.object(server_app.time, "time", return_value=1_700_000_000.25),
        ):
            result = await server_app.latency_probe("player-1")

        self.assertEqual(result, {"ok": True, "serverTimeMs": 1_700_000_000_250.0})
        emit.assert_not_awaited()


class PacketDelayRelayTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self) -> None:
        server_app.player_states.clear()
        server_app.registry.enter("player-1")

    def tearDown(self) -> None:
        server_app.player_states.clear()
        server_app.registry.in_world.clear()

    async def test_relays_sent_at_without_storing_it(self) -> None:
        emit = AsyncMock()

        with patch.object(server_app.sio, "emit", new=emit):
            result = await server_app.player_update(
                "player-1", {**valid_state(), "sentAtMs": 1_700_000_000_250.5}
            )

        self.assertEqual(result, {"ok": True})
        emit.assert_awaited_once_with(
            "player:state",
            {"playerId": "player-1", **valid_state(), "sentAtMs": 1_700_000_000_250.5},
            skip_sid="player-1",
        )
        self.assertEqual(server_app.player_states["player-1"], valid_state())

    async def test_drops_missing_or_invalid_sent_at(self) -> None:
        for sequence, sent_at in enumerate((None, -1.0, math.inf, True, "1"), start=1):
            emit = AsyncMock()
            payload = valid_state(sequence)
            if sent_at is not None:
                payload["sentAtMs"] = sent_at

            with patch.object(server_app.sio, "emit", new=emit):
                result = await server_app.player_update("player-1", payload)

            self.assertEqual(result, {"ok": True})
            emit.assert_awaited_once_with(
                "player:state",
                {"playerId": "player-1", **valid_state(sequence)},
                skip_sid="player-1",
            )
