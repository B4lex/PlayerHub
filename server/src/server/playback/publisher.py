"""Where a replay's messages go: the same relay events a real player produces."""

import time
from collections.abc import Awaitable, Callable
from typing import Any, Protocol

from server.registry import PlayerRegistry

Emit = Callable[..., Awaitable[Any]]


class Publisher(Protocol):
    async def checkpoint(self, payload: dict[str, Any]) -> None: ...
    async def action(self, payload: dict[str, Any]) -> None: ...
    async def appearance(self, payload: dict[str, Any]) -> None: ...
    async def equipment(self, payload: dict[str, Any]) -> None: ...
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
        wall_clock: Callable[[], float] = time.time,
    ) -> None:
        self.player_id = player_id
        self._emit = emit
        self._registry = registry
        self._wall_clock = wall_clock

    async def checkpoint(self, payload: dict[str, Any]) -> None:
        outbound = {"playerId": self.player_id, **payload, "sentAtMs": self._wall_clock() * 1000.0}
        await self._emit("player:state", outbound)

    async def action(self, payload: dict[str, Any]) -> None:
        await self._emit("player:action", {"playerId": self.player_id, **payload})

    async def appearance(self, payload: dict[str, Any]) -> None:
        self._registry.appearances[self.player_id] = payload
        await self._emit("player:appearance", {"playerId": self.player_id, **payload})

    async def equipment(self, payload: dict[str, Any]) -> None:
        self._registry.equipment[self.player_id] = payload
        await self._emit("player:equipment", {"playerId": self.player_id, **payload})

    async def left(self) -> None:
        self._registry.appearances.pop(self.player_id, None)
        self._registry.equipment.pop(self.player_id, None)
        await self._emit("player:left", {"playerId": self.player_id})
