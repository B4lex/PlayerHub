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

# Sustained bow draw, spell channelling, and blocking. Kept as checkpoint state
# rather than actions because their duration matters, not how often they start.
HAND_PRESENTATION_FIELDS = ("bowDrawn", "castingLeft", "castingRight", "blocking")

ANIMATION_ACTION_FAMILIES = frozenset({"attack"})
# Right-hand, left-hand, and dual-wield swings, and bashes. Each may be a power
# attack or power bash.
ATTACK_ACTIONS = frozenset({"normal", "left", "dual", "bash"})
# The movement direction a power attack was started with. Only power attacks
# carry one.
POWER_ATTACK_DIRECTIONS = frozenset({"standing", "forward", "backward", "left", "right"})

MAXIMUM_INT64 = 0x7FFFFFFFFFFFFFFF
MAXIMUM_PLUGIN_NAME_LENGTH = 260
MAXIMUM_LOCAL_FORM_ID = 0xFFFFFF
MAXIMUM_HEAD_PARTS = 16
MAXIMUM_HEAD_PART_TYPE = 6
MAXIMUM_TINTS = 64
MAXIMUM_TINT_TYPE = 14
FACE_MORPH_COUNT = 19
FACE_PART_COUNT = 4
MAXIMUM_NAME_BYTES = 128
MAXIMUM_FLOAT32 = 3.4028234663852886e38


def parse_player_update(data: Any) -> dict[str, bool | int | float | str] | None:
    if not isinstance(data, dict):
        return None

    numeric_fields = ("x", "y", "z", "heading", "speed", "direction")
    values: dict[str, bool | int | float | str] = {}
    for field in numeric_fields:
        value = data.get(field)
        if isinstance(value, bool) or not isinstance(value, (int, float)):
            return None
        if not math.isfinite(value):
            return None
        if field == "speed" and value < 0:
            return None
        # Movement direction relative to heading, in behavior-graph units.
        if field == "direction" and not 0 <= value <= 1:
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

    # Recordings made before bow, spell, and block synchronization have no hand
    # presentation, which means nothing is drawn, cast, or blocked.
    for field in HAND_PRESENTATION_FIELDS:
        presenting = data.get(field, False)
        if not isinstance(presenting, bool):
            return None
        values[field] = presenting

    # Dying or dead. Recordings made before death synchronization have no life
    # state, which means the player is alive.
    dead = data.get("dead", False)
    if not isinstance(dead, bool):
        return None
    values["dead"] = dead

    # Riding a mount, whose transform the position fields then carry. Recordings
    # made before mount synchronization have no mount state, which means on foot.
    mounted = data.get("mounted", False)
    if not isinstance(mounted, bool):
        return None
    values["mounted"] = mounted

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


def parse_sent_at(data: Any) -> float | None:
    """Return the optional server-clock emit time a client stamped on a payload."""
    if not isinstance(data, dict):
        return None
    sent_at = data.get("sentAtMs")
    if (
        isinstance(sent_at, bool)
        or not isinstance(sent_at, (int, float))
        or not math.isfinite(sent_at)
        or sent_at < 0
    ):
        return None
    return float(sent_at)


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

    power = data.get("power")
    if not isinstance(power, bool):
        return None
    direction = data.get("direction")
    if power:
        if not isinstance(direction, str) or direction not in POWER_ATTACK_DIRECTIONS:
            return None
    elif "direction" in data:
        return None

    prerequisites = data.get("prerequisites")
    if not isinstance(prerequisites, dict) or prerequisites != {"weaponDrawn": True}:
        return None

    parsed = {
        "actionSequence": action_sequence,
        "graphEpoch": graph_epoch,
        "checkpointSequence": checkpoint_sequence,
        "family": family,
        "action": action,
        "power": power,
        "prerequisites": prerequisites.copy(),
    }
    if power:
        parsed["direction"] = direction
    return parsed


def _is_integer(value: Any, minimum: int, maximum: int) -> bool:
    return (
        not isinstance(value, bool)
        and isinstance(value, int)
        and minimum <= value <= maximum
    )


def _is_number(value: Any, minimum: float, maximum: float) -> bool:
    return (
        not isinstance(value, bool)
        and isinstance(value, (int, float))
        and math.isfinite(value)
        and minimum <= value <= maximum
    )


def _parse_form_reference(data: Any, extra_fields: tuple[str, ...] = ()) -> dict[str, Any] | None:
    if not isinstance(data, dict):
        return None
    plugin = data.get("plugin")
    form_id = data.get("formId")
    if (
        not isinstance(plugin, str)
        or not 1 <= len(plugin) <= MAXIMUM_PLUGIN_NAME_LENGTH
        or not _is_integer(form_id, 0, MAXIMUM_LOCAL_FORM_ID)
    ):
        return None
    reference: dict[str, Any] = {"plugin": plugin, "formId": form_id}
    for field in extra_fields:
        reference[field] = data.get(field)
    return reference


def _parse_color(data: Any, extra_fields: tuple[str, ...] = ()) -> dict[str, Any] | None:
    if not isinstance(data, dict):
        return None
    color: dict[str, Any] = {}
    for channel in ("r", "g", "b"):
        value = data.get(channel)
        if not _is_integer(value, 0, 0xFF):
            return None
        color[channel] = value
    for field in extra_fields:
        color[field] = data.get(field)
    return color


def _parse_name(data: Any) -> str | None:
    # Recordings made before name synchronization have no name.
    if data is None:
        return ""
    if not isinstance(data, str) or any(ord(c) < 0x20 or ord(c) == 0x7F for c in data):
        return None
    try:
        encoded = data.encode("utf-8")
    except UnicodeEncodeError:
        return None
    return data if len(encoded) <= MAXIMUM_NAME_BYTES else None


def parse_player_appearance(data: Any) -> dict[str, Any] | None:
    """Validate latest-wins character appearance state."""
    if not isinstance(data, dict):
        return None

    revision = data.get("revision")
    female = data.get("female")
    weight = data.get("weight")
    height = data.get("height")
    if (
        not _is_integer(revision, 1, MAXIMUM_INT64)
        or not isinstance(female, bool)
        or not _is_number(weight, 0.0, 100.0)
        or not _is_number(height, 0.25, 4.0)
    ):
        return None

    name = _parse_name(data.get("name"))
    race = _parse_form_reference(data.get("race"))
    if name is None or race is None:
        return None

    raw_head_parts = data.get("headParts")
    if not isinstance(raw_head_parts, list) or len(raw_head_parts) > MAXIMUM_HEAD_PARTS:
        return None
    head_parts = []
    for raw_head_part in raw_head_parts:
        head_part = _parse_form_reference(raw_head_part, ("type",))
        if head_part is None or not _is_integer(head_part["type"], 0, MAXIMUM_HEAD_PART_TYPE):
            return None
        head_parts.append(head_part)

    if "hairColor" not in data or "faceDetails" not in data:
        return None
    hair_color = None
    if data["hairColor"] is not None:
        hair_color = _parse_color(data["hairColor"])
        if hair_color is None:
            return None
    face_details = None
    if data["faceDetails"] is not None:
        face_details = _parse_form_reference(data["faceDetails"])
        if face_details is None:
            return None

    body_tint = _parse_color(data.get("bodyTint"))
    if body_tint is None:
        return None

    face_morphs = data.get("faceMorphs")
    if (
        not isinstance(face_morphs, list)
        or len(face_morphs) != FACE_MORPH_COUNT
        or not all(_is_number(value, -MAXIMUM_FLOAT32, MAXIMUM_FLOAT32) for value in face_morphs)
    ):
        return None

    face_parts = data.get("faceParts")
    if (
        not isinstance(face_parts, list)
        or len(face_parts) != FACE_PART_COUNT
        or not all(_is_integer(value, -(2**31), 2**31 - 1) for value in face_parts)
    ):
        return None

    raw_tints = data.get("tints")
    if not isinstance(raw_tints, list) or len(raw_tints) > MAXIMUM_TINTS:
        return None
    tints = []
    for raw_tint in raw_tints:
        tint = _parse_color(raw_tint, ("index", "type", "alpha"))
        if (
            tint is None
            or not _is_integer(tint["index"], 0, 0xFFFF)
            or not _is_integer(tint["type"], 0, MAXIMUM_TINT_TYPE)
            or not _is_number(tint["alpha"], 0.0, 1.0)
        ):
            return None
        tints.append(tint)

    return {
        "revision": revision,
        "name": name,
        "race": race,
        "female": female,
        "weight": weight,
        "height": height,
        "headParts": head_parts,
        "hairColor": hair_color,
        "faceDetails": face_details,
        "bodyTint": body_tint,
        "faceMorphs": list(face_morphs),
        "faceParts": list(face_parts),
        "tints": tints,
    }


MAXIMUM_ARMOR_ENTRIES = 32
ARMOR_TYPES = frozenset({"light", "heavy", "clothing"})
HAND_KINDS = frozenset({"weapon", "torch", "spell"})
MINIMUM_WEAPON_TYPE = 1
MAXIMUM_WEAPON_TYPE = 9
TWO_HANDED_WEAPON_TYPES = frozenset({5, 6, 7, 9})


def _parse_hand(data: Any, left_hand: bool) -> dict[str, Any] | None:
    # A spell hand has no item identity: every receiver shows the same
    # placeholder spell, so the source's spell is never sent.
    if isinstance(data, dict) and data.get("kind") == "spell":
        return {"kind": "spell"}
    reference = _parse_form_reference(data, ("kind",))
    if reference is None or reference["kind"] not in HAND_KINDS:
        return None
    if reference["kind"] == "torch":
        return reference if left_hand else None

    weapon_type = data.get("weaponType")
    if not _is_integer(weapon_type, MINIMUM_WEAPON_TYPE, MAXIMUM_WEAPON_TYPE):
        return None
    if left_hand and weapon_type in TWO_HANDED_WEAPON_TYPES:
        return None
    reference["weaponType"] = weapon_type
    return reference


def parse_player_equipment(data: Any) -> dict[str, Any] | None:
    """Validate latest-wins equipped item state."""
    if not isinstance(data, dict):
        return None

    revision = data.get("revision")
    if not _is_integer(revision, 1, MAXIMUM_INT64):
        return None

    raw_armor = data.get("armor")
    if not isinstance(raw_armor, list) or len(raw_armor) > MAXIMUM_ARMOR_ENTRIES:
        return None
    armor = []
    seen_armor: set[tuple[str, int]] = set()
    for raw_entry in raw_armor:
        entry = _parse_form_reference(raw_entry, ("slotMask", "armorType"))
        if (
            entry is None
            or not _is_integer(entry["slotMask"], 1, 0xFFFFFFFF)
            or entry["armorType"] not in ARMOR_TYPES
        ):
            return None
        key = (entry["plugin"], entry["formId"])
        if key in seen_armor:
            return None
        seen_armor.add(key)
        armor.append(entry)

    if "rightHand" not in data or "leftHand" not in data:
        return None
    right_hand = None
    if data["rightHand"] is not None:
        right_hand = _parse_hand(data["rightHand"], left_hand=False)
        if right_hand is None:
            return None
    left_hand = None
    if data["leftHand"] is not None:
        left_hand = _parse_hand(data["leftHand"], left_hand=True)
        if left_hand is None:
            return None
    if (
        right_hand is not None
        and right_hand["kind"] == "weapon"
        and right_hand["weaponType"] in TWO_HANDED_WEAPON_TYPES
        and left_hand is not None
    ):
        return None

    # Recordings made before ammo synchronization have no ammo.
    ammo = None
    if data.get("ammo") is not None:
        ammo = _parse_form_reference(data["ammo"])
        if ammo is None:
            return None

    return {
        "revision": revision,
        "armor": armor,
        "rightHand": right_hand,
        "leftHand": left_hand,
        "ammo": ammo,
    }
