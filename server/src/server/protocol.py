import math
from typing import Any

MOVEMENT_STATES = frozenset(
    {
        "idle",
        "walking",
        "running",
        "sprinting",
        "sneakingIdle",
        "sneakingWalking",
        "sneakingRunning",
    }
)

ANIMATION_ACTION_FAMILIES = frozenset({"attack"})
ATTACK_ACTIONS = frozenset({"normal"})


def parse_player_update(data: Any) -> dict[str, bool | int | float | str] | None:
    if not isinstance(data, dict):
        return None

    numeric_fields = ("x", "y", "z", "heading", "speed")
    values: dict[str, bool | int | float | str] = {}
    for field in numeric_fields:
        value = data.get(field)
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            return None
        if not math.isfinite(value):
            return None
        if field == "speed" and value < 0:
            return None
        values[field] = value

    movement_state = data.get("movementState")
    if movement_state not in MOVEMENT_STATES:
        return None
    values["movementState"] = movement_state

    jumping = data.get("jumping")
    if not isinstance(jumping, bool):
        return None
    values["jumping"] = jumping

    weapon_drawn = data.get("weaponDrawn")
    if not isinstance(weapon_drawn, bool):
        return None
    values["weaponDrawn"] = weapon_drawn

    cell_id = data.get("cellId")
    worldspace_id = data.get("worldspaceId")
    furniture_ref_id = data.get("furnitureRefId")
    graph_epoch = data.get("graphEpoch")
    sequence = data.get("sequence")
    if (
        isinstance(cell_id, bool)
        or not isinstance(cell_id, int)
        or not 0 <= cell_id <= 0xFFFFFFFF
        or isinstance(worldspace_id, bool)
        or not isinstance(worldspace_id, int)
        or not 0 <= worldspace_id <= 0xFFFFFFFF
        or isinstance(furniture_ref_id, bool)
        or not isinstance(furniture_ref_id, int)
        or not 0 <= furniture_ref_id <= 0xFFFFFFFF
        or isinstance(graph_epoch, bool)
        or not isinstance(graph_epoch, int)
        or not 1 <= graph_epoch <= 0x7FFFFFFFFFFFFFFF
        or isinstance(sequence, bool)
        or not isinstance(sequence, int)
        or not 0 <= sequence <= 0x7FFFFFFFFFFFFFFF
    ):
        return None

    values["cellId"] = cell_id
    values["worldspaceId"] = worldspace_id
    values["furnitureRefId"] = furniture_ref_id
    values["graphEpoch"] = graph_epoch
    values["sequence"] = sequence
    return values


def parse_animation_action(data: Any) -> dict[str, Any] | None:
    """Validate the deliberately small, versioned semantic action vocabulary."""
    if not isinstance(data, dict):
        return None

    action_sequence = data.get("actionSequence")
    graph_epoch = data.get("graphEpoch")
    checkpoint_sequence = data.get("checkpointSequence")
    if any(
        isinstance(value, bool)
        or not isinstance(value, int)
        or not 1 <= value <= 0x7FFFFFFFFFFFFFFF
        for value in (action_sequence, graph_epoch, checkpoint_sequence)
    ):
        return None

    family = data.get("family")
    action = data.get("action")
    if (
        not isinstance(family, str)
        or family not in ANIMATION_ACTION_FAMILIES
        or not isinstance(action, str)
        or action not in ATTACK_ACTIONS
    ):
        return None

    prerequisites = data.get("prerequisites")
    if not isinstance(prerequisites, dict) or prerequisites != {"weaponDrawn": True}:
        return None

    return {
        "actionSequence": action_sequence,
        "graphEpoch": graph_epoch,
        "checkpointSequence": checkpoint_sequence,
        "family": family,
        "action": action,
        "prerequisites": prerequisites.copy(),
    }
