import asyncio
import unittest
from unittest.mock import AsyncMock, call

from server.relay import CheckpointRelay


def state(player_id: str, sequence: int) -> dict[str, object]:
    return {"playerId": player_id, "sequence": sequence}


class CheckpointRelayTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self) -> None:
        self.emit = AsyncMock()
        self.relay = CheckpointRelay(self.emit)

    async def test_sends_the_newest_checkpoint_of_every_player_in_one_batch(self) -> None:
        self.relay.publish("player-1", state("player-1", 1))
        self.relay.publish("player-2", state("player-2", 7))
        self.relay.publish("player-1", state("player-1", 2))

        await self.relay.flush()
        await self.relay.flush()

        self.emit.assert_awaited_once_with(
            "player:states", {"states": [state("player-1", 2), state("player-2", 7)]}
        )

    async def test_flushing_a_player_sends_only_its_checkpoint(self) -> None:
        self.relay.publish("player-1", state("player-1", 1))
        self.relay.publish("player-2", state("player-2", 1))

        await self.relay.flush_player("player-1")
        await self.relay.flush_player("player-1")
        await self.relay.flush()

        self.assertEqual(
            self.emit.await_args_list,
            [
                call("player:states", {"states": [state("player-1", 1)]}),
                call("player:states", {"states": [state("player-2", 1)]}),
            ],
        )

    async def test_discarded_checkpoints_are_never_sent(self) -> None:
        self.relay.publish("player-1", state("player-1", 1))
        self.relay.discard("player-1")
        self.relay.discard("player-1")

        await self.relay.flush()

        self.emit.assert_not_awaited()

    async def test_ticks_until_stopped(self) -> None:
        relay = CheckpointRelay(self.emit, interval=0.01)
        relay.start()
        relay.start()
        relay.publish("player-1", state("player-1", 1))
        await asyncio.sleep(0.05)
        relay.publish("player-1", state("player-1", 2))
        await asyncio.sleep(0.05)

        await relay.stop()
        await relay.stop()
        relay.publish("player-1", state("player-1", 3))
        await asyncio.sleep(0.03)

        self.assertEqual(
            self.emit.await_args_list,
            [
                call("player:states", {"states": [state("player-1", 1)]}),
                call("player:states", {"states": [state("player-1", 2)]}),
            ],
        )

    async def test_a_failed_send_does_not_stop_the_ticks(self) -> None:
        self.emit.side_effect = [RuntimeError("socket gone"), None]
        relay = CheckpointRelay(self.emit, interval=0.01)
        relay.start()
        self.addAsyncCleanup(relay.stop)

        relay.publish("player-1", state("player-1", 1))
        with self.assertLogs("uvicorn.error", "ERROR"):
            await asyncio.sleep(0.05)
        relay.publish("player-1", state("player-1", 2))
        await asyncio.sleep(0.05)

        self.assertEqual(self.emit.await_count, 2)
        self.emit.assert_awaited_with("player:states", {"states": [state("player-1", 2)]})


if __name__ == "__main__":
    unittest.main()
