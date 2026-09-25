"""A bot that reaches the server as a real Socket.IO client, like the plugin does."""

from typing import Any

import socketio

CONNECT_TIMEOUT_SECONDS = 10.0


class ClientPublisher:
    """Plays a replay through the server's player ingestion path.

    Unlike a virtual player, every message is parsed, validated, and relayed by
    the same handlers that serve real players, and the connection is a real peer
    the server pings and sends every other player's state to.
    """

    def __init__(self, client: socketio.AsyncClient) -> None:
        self._client = client
        self._in_world = False
        self.player_id: str = client.get_sid() or ""

    @classmethod
    async def connect(cls, url: str) -> "ClientPublisher":
        client = socketio.AsyncClient(reconnection=False)
        await client.connect(url, transports=["websocket"], wait_timeout=CONNECT_TIMEOUT_SECONDS)
        return cls(client)

    async def checkpoint(self, payload: dict[str, Any]) -> None:
        if not self._in_world:
            await self._emit("player:enter", {})
            self._in_world = True
        await self._emit("player:update", payload)

    async def action(self, payload: dict[str, Any]) -> None:
        await self._emit("animation:action", payload)

    async def mount_transition(self, payload: dict[str, Any]) -> None:
        await self._emit("mount:transition", payload)

    async def appearance(self, payload: dict[str, Any]) -> None:
        await self._emit("player:appearance", payload)

    async def equipment(self, payload: dict[str, Any]) -> None:
        await self._emit("player:equipment", payload)

    async def exit(self) -> None:
        self._in_world = False
        await self._emit("player:exit", {})

    async def left(self) -> None:
        await self._client.disconnect()

    async def _emit(self, event: str, payload: dict[str, Any]) -> None:
        if not self._client.connected:
            raise ConnectionError("disconnected from the server")
        await self._client.emit(event, payload)
