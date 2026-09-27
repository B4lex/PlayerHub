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

    A connected player is in the world only between its ``player:enter`` and
    ``player:exit``; outside it (main menu, loading screens) it has no
    checkpoint or action stream, but keeps its appearance and equipment.
    """

    connected_at: dict[str, float] = field(default_factory=dict)
    in_world: set[str] = field(default_factory=set)
    states: dict[str, PlayerUpdate] = field(default_factory=dict)
    action_sequences: dict[str, int] = field(default_factory=dict)
    # Checkpoint sequence of the latest relayed mount transition.
    mount_transition_sequences: dict[str, int] = field(default_factory=dict)
    appearances: dict[str, dict[str, Any]] = field(default_factory=dict)
    # Tells apart players with the same name: the first holder of a name gets 1,
    # later ones the lowest index free among them. It is kept while the name is.
    name_indices: dict[str, int] = field(default_factory=dict)
    equipment: dict[str, dict[str, Any]] = field(default_factory=dict)

    def add(self, player_id: str) -> None:
        self.connected_at[player_id] = time.time()

    def remove(self, player_id: str) -> None:
        self.connected_at.pop(player_id, None)
        self.exit(player_id)
        self.forget_presentation(player_id)

    def forget_presentation(self, player_id: str) -> None:
        self.appearances.pop(player_id, None)
        self.name_indices.pop(player_id, None)
        self.equipment.pop(player_id, None)

    def enter(self, player_id: str) -> bool:
        """Mark the player as in the world. False if it already was."""
        if player_id in self.in_world:
            return False
        self.in_world.add(player_id)
        return True

    def exit(self, player_id: str) -> bool:
        """Drop the player's world state. False if it was not in the world."""
        self.states.pop(player_id, None)
        self.action_sequences.pop(player_id, None)
        self.mount_transition_sequences.pop(player_id, None)
        if player_id not in self.in_world:
            return False
        self.in_world.discard(player_id)
        return True

    def set_appearance(self, player_id: str, appearance: dict[str, Any]) -> dict[str, Any]:
        """Store the appearance and return it as relayed to peers."""
        previous = self.appearances.get(player_id)
        self.appearances[player_id] = appearance
        name = appearance["name"].casefold()
        if previous is None or previous["name"].casefold() != name:
            taken = {
                self.name_indices.get(other, 1)
                for other, other_appearance in self.appearances.items()
                if other != player_id and other_appearance["name"].casefold() == name
            }
            self.name_indices[player_id] = next(
                index for index in range(1, len(taken) + 2) if index not in taken
            )
        return self.relayed_appearance(player_id)

    def relayed_appearance(self, player_id: str) -> dict[str, Any]:
        return {**self.appearances[player_id], "nameIndex": self.name_indices.get(player_id, 1)}

    def name_of(self, player_id: str) -> str:
        appearance = self.appearances.get(player_id)
        return appearance["name"] if appearance is not None else ""

    def sender_of(self, player_id: str) -> dict[str, Any]:
        """How chat names the player; a zero level is unknown."""
        appearance = self.appearances.get(player_id, {})
        return {
            "name": appearance.get("name", ""),
            "nameIndex": self.name_indices.get(player_id, 1),
            "level": appearance.get("level", 0),
        }

    def initial_events(self, player_id: str) -> list[tuple[str, dict[str, Any]]]:
        """The player's current state as events, in the order a peer needs them."""
        events: list[tuple[str, dict[str, Any]]] = []
        if player_id not in self.in_world:
            events.append(("player:exit", {}))
        if player_id in self.appearances:
            events.append(("player:appearance", self.appearances[player_id]))
        if player_id in self.equipment:
            events.append(("player:equipment", self.equipment[player_id]))
        if player_id in self.states:
            events.append(("player:update", self.states[player_id]))
        return events
