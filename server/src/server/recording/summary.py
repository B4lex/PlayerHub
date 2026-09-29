"""Notable moments of a recording, for the admin timeline."""

from dataclasses import dataclass

from server.recording.format import Recording

_ATTACK_NAMES = {
    "normal": "attack",
    "left": "left-hand attack",
    "dual": "dual attack",
    "bash": "bash",
}


@dataclass(frozen=True)
class Marker:
    t_ms: int
    # "attack" | "appearance" | "equipment" | "weapon" | "furniture" | "area"
    kind: str
    label: str


def _area_key(payload: dict) -> tuple[str, int]:
    # Exterior cells change constantly while walking; only the worldspace matters there.
    if payload["worldspaceId"] != 0:
        return ("worldspace", payload["worldspaceId"])
    return ("interior", payload["cellId"])


def summarize_markers(recording: Recording) -> list[Marker]:
    """Attacks, gear changes, drawing or sheathing, furniture use, and area changes."""
    markers: list[Marker] = []
    seen_appearance = False
    seen_equipment = False
    previous: dict | None = None

    for message in recording.messages:
        payload = message.payload
        if message.event == "animation:action":
            name = _ATTACK_NAMES.get(payload["action"], payload["action"])
            if payload["power"]:
                label = f"Power {name} ({payload['direction']})"
            else:
                label = name.capitalize()
            markers.append(Marker(message.t_ms, "attack", label))
        elif message.event == "player:appearance":
            if seen_appearance:
                markers.append(Marker(message.t_ms, "appearance", "Appearance changed"))
            seen_appearance = True
        elif message.event == "player:equipment":
            if seen_equipment:
                markers.append(Marker(message.t_ms, "equipment", "Equipment changed"))
            seen_equipment = True
        elif message.event == "player:update":
            if previous is not None:
                if payload["weaponDrawn"] != previous["weaponDrawn"]:
                    label = "Weapon drawn" if payload["weaponDrawn"] else "Weapon sheathed"
                    markers.append(Marker(message.t_ms, "weapon", label))
                if (payload["furniture"] is None) != (previous["furniture"] is None):
                    label = "Left furniture" if payload["furniture"] is None else "Started using furniture"
                    markers.append(Marker(message.t_ms, "furniture", label))
                if _area_key(payload) != _area_key(previous):
                    kind, area_id = _area_key(payload)
                    markers.append(
                        Marker(message.t_ms, "area", f"Entered {kind} {area_id:#010x}")
                    )
            previous = payload
    return markers
