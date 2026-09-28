import unittest
from unittest.mock import AsyncMock, patch

from recording_fixtures import checkpoint, mount_transition

import server.app as server_app


class MountTransitionRelayTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self) -> None:
        server_app.player_states.clear()
        server_app.player_mount_transition_sequences.clear()
        server_app.registry.in_world.clear()
        server_app.checkpoints._pending.clear()

    def tearDown(self) -> None:
        self.setUp()

    async def test_relays_transition_against_mounted_checkpoint(self) -> None:
        emit = AsyncMock()
        server_app.player_states["player-1"] = checkpoint(10, mounted=True)

        with patch.object(server_app.sio, "emit", new=emit):
            result = await server_app.mount_transition("player-1", mount_transition("enter", 10))

        self.assertEqual(result, {"ok": True})
        emit.assert_awaited_once_with(
            "player:mount",
            {"playerId": "player-1", **mount_transition("enter", 10)},
            skip_sid="player-1",
        )

    async def test_relays_enter_then_exit(self) -> None:
        emit = AsyncMock()

        with patch.object(server_app.sio, "emit", new=emit):
            server_app.player_states["player-1"] = checkpoint(10, mounted=True)
            entered = await server_app.mount_transition("player-1", mount_transition("enter", 10))
            server_app.player_states["player-1"] = checkpoint(80, mounted=True)
            exited = await server_app.mount_transition("player-1", mount_transition("exit", 80))

        self.assertEqual((entered, exited), ({"ok": True}, {"ok": True}))
        self.assertEqual(
            [call.args[1]["transition"] for call in emit.await_args_list],
            ["enter", "exit"],
        )

    async def test_rejects_malformed_transition(self) -> None:
        server_app.player_states["player-1"] = checkpoint(10, mounted=True)

        result = await server_app.mount_transition("player-1", mount_transition("jump", 10))

        self.assertEqual(result, {"ok": False, "error": "invalid mount transition"})

    async def test_rejects_transition_without_compatible_checkpoint(self) -> None:
        cases = {
            "no checkpoint": (None, mount_transition("enter", 10)),
            "on foot": (checkpoint(10), mount_transition("enter", 10)),
            "dead": (checkpoint(10, mounted=True, dead=True), mount_transition("exit", 10)),
            "old epoch": (checkpoint(10, mounted=True, graphEpoch=2), mount_transition("enter", 10)),
            "future checkpoint": (checkpoint(10, mounted=True), mount_transition("enter", 11)),
        }
        emit = AsyncMock()
        for name, (state, transition) in cases.items():
            with self.subTest(name):
                server_app.player_states.clear()
                if state is not None:
                    server_app.player_states["player-1"] = state

                with patch.object(server_app.sio, "emit", new=emit):
                    result = await server_app.mount_transition("player-1", transition)

                self.assertEqual(result, {"ok": False, "error": "incompatible mount checkpoint"})
        emit.assert_not_awaited()

    async def test_rejects_repeated_or_reordered_transition(self) -> None:
        server_app.player_states["player-1"] = checkpoint(12, mounted=True)
        emit = AsyncMock()

        with patch.object(server_app.sio, "emit", new=emit):
            first = await server_app.mount_transition("player-1", mount_transition("enter", 11))
            repeated = await server_app.mount_transition("player-1", mount_transition("exit", 11))
            reordered = await server_app.mount_transition("player-1", mount_transition("exit", 10))

        self.assertEqual(first, {"ok": True})
        self.assertEqual(repeated, {"ok": False, "error": "stale mount transition"})
        self.assertEqual(reordered, {"ok": False, "error": "stale mount transition"})
        emit.assert_awaited_once()

    async def test_world_exit_forgets_transition_order(self) -> None:
        server_app.registry.enter("player-1")
        server_app.player_states["player-1"] = checkpoint(40, mounted=True)
        server_app.player_mount_transition_sequences["player-1"] = 40

        with patch.object(server_app.sio, "emit", new=AsyncMock()):
            await server_app.player_exit("player-1")

        self.assertNotIn("player-1", server_app.player_mount_transition_sequences)
