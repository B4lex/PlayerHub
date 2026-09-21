"""Per-connection player state shared by the sync handlers and the admin layer."""

import time
from dataclasses import dataclass, field
from typing import Any

PlayerUpdate = dict[str, bool | int | float | str]


@dataclass
class PlayerRegistry:
    """Latest accepted state of every connected player, keyed by Socket.IO session ID.

    Replay playback also files its synthetic players' appearance and equipment
    here, so a client that joins mid-replay receives them like any other peer's.
    """

    connected_at: dict[str, float] = field(default_factory=dict)
    states: dict[str, PlayerUpdate] = field(default_factory=dict)
    action_sequences: dict[str, int] = field(default_factory=dict)
    appearances: dict[str, dict[str, Any]] = field(default_factory=dict)
    equipment: dict[str, dict[str, Any]] = field(default_factory=dict)

    def add(self, player_id: str) -> None:
        self.connected_at[player_id] = time.time()

    def remove(self, player_id: str) -> None:
        self.connected_at.pop(player_id, None)
        self.states.pop(player_id, None)
        self.action_sequences.pop(player_id, None)
        self.appearances.pop(player_id, None)
        self.equipment.pop(player_id, None)

    def name_of(self, player_id: str) -> str:
        appearance = self.appearances.get(player_id)
        return appearance["name"] if appearance is not None else ""

    def initial_events(self, player_id: str) -> list[tuple[str, dict[str, Any]]]:
        """The player's current state as events, in the order a peer needs them."""
        events: list[tuple[str, dict[str, Any]]] = []
        if player_id in self.appearances:
            events.append(("player:appearance", self.appearances[player_id]))
        if player_id in self.equipment:
            events.append(("player:equipment", self.equipment[player_id]))
        if player_id in self.states:
            events.append(("player:update", self.states[player_id]))
        return events
