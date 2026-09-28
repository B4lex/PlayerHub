"""Turns recorded payloads into the live message stream of one synthetic peer.

Receivers accept a peer's checkpoint only with a rising ``sequence`` and a
non-decreasing ``graphEpoch``, its actions only with contiguous
``actionSequence`` values, and appearance or equipment only with rising
revisions. A recording's own numbers stop satisfying that as soon as playback
seeks backwards, loops, or skips ahead, so the virtual player numbers
everything itself and only borrows the recording's content.
"""

from typing import Any

PlayerPayload = dict[str, Any]

_SNEAKING_STATES = frozenset({"sneakingIdle", "sneakingWalking", "sneakingRunning"})


def hold_pose(update: PlayerPayload) -> PlayerPayload:
    """The same checkpoint with all locomotion stopped.

    A paused replay keeps sending its last checkpoint so peers that join later
    still see the actor. Sending it unchanged would leave the remote actor
    running in place, so it stands still instead, keeping stance and equipment.
    """
    held = dict(update)
    held["movementState"] = (
        "sneakingIdle" if update["movementState"] in _SNEAKING_STATES else "idle"
    )
    held["speed"] = 0.0
    held["jumping"] = False
    return held


def _content(payload: PlayerPayload) -> PlayerPayload:
    return {key: value for key, value in payload.items() if key != "revision"}


class VirtualPlayer:
    """Numbering state for one replayed player.

    Every method returns a payload ready to relay (without ``playerId``), or
    ``None`` when there is nothing new to say.
    """

    def __init__(self) -> None:
        self._sequence = 0
        self._action_sequence = 0
        self._appearance_revision = 0
        self._equipment_revision = 0
        self._epoch = 0
        self._epoch_offset = 0
        self._epoch_break = False
        self._appearance: PlayerPayload | None = None
        self._equipment: PlayerPayload | None = None

    def break_continuity(self) -> None:
        """Mark that the next checkpoint does not follow from the previous one.

        Called when playback jumps in time. The next checkpoint starts a new
        graph epoch, which tells receivers to drop queued animation actions
        that belonged to the old position instead of playing them late.
        """
        self._epoch_break = True

    def checkpoint(self, recorded: PlayerPayload, *, hold: bool = False) -> PlayerPayload:
        update = hold_pose(recorded) if hold else dict(recorded)
        recorded_epoch = update["graphEpoch"]
        if self._epoch_break and self._epoch:
            self._epoch_offset = self._epoch + 1 - recorded_epoch
        self._epoch_break = False
        self._epoch = max(recorded_epoch + self._epoch_offset, self._epoch, 1)
        self._sequence += 1
        update["graphEpoch"] = self._epoch
        update["sequence"] = self._sequence
        return update

    def action(self, recorded: PlayerPayload) -> PlayerPayload:
        if self._sequence == 0:
            raise ValueError("an action needs a checkpoint to follow")
        self._action_sequence += 1
        return {
            **recorded,
            "actionSequence": self._action_sequence,
            "graphEpoch": self._epoch,
            "checkpointSequence": self._sequence,
        }

    def appearance(self, recorded: PlayerPayload) -> PlayerPayload | None:
        # Receivers rebuild the actor for a new appearance, so an unchanged one
        # (for example after seeking within the same look) is not resent.
        content = _content(recorded)
        if content == self._appearance:
            return None
        self._appearance = content
        self._appearance_revision += 1
        return {**recorded, "revision": self._appearance_revision}

    def equipment(self, recorded: PlayerPayload) -> PlayerPayload | None:
        content = _content(recorded)
        if content == self._equipment:
            return None
        self._equipment = content
        self._equipment_revision += 1
        return {**recorded, "revision": self._equipment_revision}
