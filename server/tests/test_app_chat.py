import unittest
from unittest.mock import AsyncMock, patch

import server.app as server_app
from server.chat import ChatThrottle


class ChatRelayTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self) -> None:
        self.clear()
        self.addCleanup(self.clear)

    @staticmethod
    def clear() -> None:
        server_app.registry.appearances.clear()
        server_app.chat_throttle._sent.clear()

    async def test_relays_to_every_player_with_the_sender_name(self) -> None:
        server_app.registry.appearances["player-1"] = {"name": "Lydia"}
        emit = AsyncMock()

        with patch.object(server_app.sio, "emit", new=emit):
            result = await server_app.chat_send("player-1", {"text": "  I am sworn to carry your burdens "})

        self.assertEqual(result, {"ok": True})
        emit.assert_awaited_once_with(
            "chat:message",
            {"playerId": "player-1", "name": "Lydia", "text": "I am sworn to carry your burdens"},
        )

    async def test_relays_an_empty_name_before_appearance_is_known(self) -> None:
        emit = AsyncMock()

        with patch.object(server_app.sio, "emit", new=emit):
            result = await server_app.chat_send("player-1", {"text": "hello"})

        self.assertEqual(result, {"ok": True})
        emit.assert_awaited_once_with("chat:message", {"playerId": "player-1", "name": "", "text": "hello"})

    async def test_rejects_malformed_messages_without_relaying(self) -> None:
        emit = AsyncMock()

        with patch.object(server_app.sio, "emit", new=emit):
            result = await server_app.chat_send("player-1", {"text": "   "})

        self.assertEqual(result, {"ok": False, "error": "invalid chat message"})
        emit.assert_not_awaited()

    async def test_rate_limits_each_player_separately(self) -> None:
        emit = AsyncMock()

        with patch.object(server_app.sio, "emit", new=emit):
            for _ in range(server_app.chat_throttle.limit):
                self.assertEqual(await server_app.chat_send("player-1", {"text": "spam"}), {"ok": True})
            limited = await server_app.chat_send("player-1", {"text": "spam"})
            other = await server_app.chat_send("player-2", {"text": "hi"})

        self.assertEqual(limited, {"ok": False, "error": "rate limited"})
        self.assertEqual(other, {"ok": True})
        self.assertEqual(emit.await_count, server_app.chat_throttle.limit + 1)

    async def test_disconnect_forgets_the_rate_limit(self) -> None:
        with patch.object(server_app.sio, "emit", new=AsyncMock()):
            for _ in range(server_app.chat_throttle.limit):
                await server_app.chat_send("player-1", {"text": "spam"})
            await server_app.disconnect("player-1", "client disconnect")
            result = await server_app.chat_send("player-1", {"text": "back"})

        self.assertEqual(result, {"ok": True})


class ChatThrottleTests(unittest.TestCase):
    def test_allows_again_once_the_window_passes(self) -> None:
        now = [100.0]
        throttle = ChatThrottle(limit=2, window_seconds=5.0, clock=lambda: now[0])

        self.assertTrue(throttle.allow("player-1"))
        self.assertTrue(throttle.allow("player-1"))
        self.assertFalse(throttle.allow("player-1"))
        now[0] += 4.9
        self.assertFalse(throttle.allow("player-1"))
        now[0] += 0.1
        self.assertTrue(throttle.allow("player-1"))
