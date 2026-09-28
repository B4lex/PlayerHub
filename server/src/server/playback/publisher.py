"""Where a replay's messages go: the same relay events a real player produces."""

from collections.abc import Awaitable, Callable
from typing import Any, Protocol

from server.registry import PlayerRegistry
from server.relay import CheckpointRelay

Emit = Callable[..., Awaitable[Any]]


class Publisher(Protocol):
    async def checkpoint(self, payload: dict[str, Any]) -> None: ...
    async def action(self, payload: dict[str, Any]) -> None: ...
    async def appearance(self, payload: dict[str, Any]) -> None: ...
    async def equipment(self, payload: dict[str, Any]) -> None: ...
    async def exit(self) -> None: ...
    async def left(self) -> None: ...


class SocketPublisher:
    """Relays a replayed player to every connected client, as if it were a peer.

    Appearance and equipment are also kept in the registry so clients that
    connect during the replay receive them, exactly as for real players.
    """

    def __init__(
        self,
        player_id: str,
        emit: Emit,
        registry: PlayerRegistry,
        checkpoints: CheckpointRelay,
    ) -> None:
        self.player_id = player_id
        self._emit = emit
        self._registry = registry
        self._checkpoints = checkpoints

    async def checkpoint(self, payload: dict[str, Any]) -> None:
        self._checkpoints.publish(self.player_id, {"playerId": self.player_id, **payload})

    async def action(self, payload: dict[str, Any]) -> None:
        await self._checkpoints.flush_player(self.player_id)
        await self._emit("player:action", {"playerId": self.player_id, **payload})

    async def appearance(self, payload: dict[str, Any]) -> None:
        relayed = self._registry.set_appearance(self.player_id, payload)
        await self._emit("player:appearance", {"playerId": self.player_id, **relayed})

    async def equipment(self, payload: dict[str, Any]) -> None:
        self._registry.equipment[self.player_id] = payload
        await self._emit("player:equipment", {"playerId": self.player_id, **payload})

    async def exit(self) -> None:
        self._checkpoints.discard(self.player_id)
        await self._emit("player:exit", {"playerId": self.player_id})

    async def left(self) -> None:
        self._checkpoints.discard(self.player_id)
        self._registry.forget_presentation(self.player_id)
        await self._emit("player:disconnect", {"playerId": self.player_id})
