import unittest

from server.protocol import (
    MAXIMUM_CHAT_TEXT_BYTES,
    parse_animation_action,
    parse_chat_message,
    parse_player_update,
)


class ParsePlayerUpdateTests(unittest.TestCase):
    def test_accepts_valid_transform(self) -> None:
        payload = {
            "x": 1.0,
            "y": 2,
            "z": -3.5,
            "heading": 0.25,
            "pitch": 0.0,
            "speed": 550.0,
            "direction": 0.0,
            "movementState": "sprinting",
            "jumping": False,
            "weaponDrawn": True,
            "bowDrawn": True,
            "castingLeft": False,
            "castingRight": True,
            "blocking": True,
            "dead": False,
            "mounted": False,
            "swimming": False,
            "cellId": 0x1234,
            "worldspaceId": 0x3C,
            "furniture": {"plugin": "Skyrim.esm", "formId": 0xA729},
            "graphEpoch": 1,
            "sequence": 7,
        }

        self.assertEqual(parse_player_update(payload), payload)

    def test_accepts_jump_animation_state(self) -> None:
        payload = {
            "x": 1.0,
            "y": 2.0,
            "z": 3.0,
            "heading": 0.25,
            "pitch": 0.0,
            "speed": 200.0,
            "direction": 0.0,
            "movementState": "running",
            "jumping": True,
            "weaponDrawn": False,
            "bowDrawn": False,
            "castingLeft": False,
            "castingRight": False,
            "blocking": False,
            "dead": False,
            "mounted": False,
            "swimming": False,
            "cellId": 0x1234,
            "worldspaceId": 0x3C,
            "furniture": None,
            "graphEpoch": 1,
            "sequence": 8,
        }

        self.assertEqual(parse_player_update(payload), payload)

    def test_rejects_non_boolean_jump_state(self) -> None:
        payload = {
            "x": 1.0,
            "y": 2.0,
            "z": 3.0,
            "heading": 0.25,
            "pitch": 0.0,
            "speed": 200.0,
            "direction": 0.0,
            "movementState": "running",
            "jumping": 1,
            "weaponDrawn": False,
            "cellId": 0x1234,
            "worldspaceId": 0x3C,
            "furniture": None,
            "graphEpoch": 1,
            "sequence": 8,
        }

        self.assertIsNone(parse_player_update(payload))

    def test_rejects_non_boolean_weapon_drawn_state(self) -> None:
        payload = {
            "x": 1.0,
            "y": 2.0,
            "z": 3.0,
            "heading": 0.25,
            "pitch": 0.0,
            "speed": 0.0,
            "direction": 0.0,
            "movementState": "idle",
            "jumping": False,
            "weaponDrawn": 1,
            "cellId": 0x1234,
            "worldspaceId": 0x3C,
            "furniture": None,
            "graphEpoch": 1,
            "sequence": 8,
        }

        self.assertIsNone(parse_player_update(payload))

    def test_rejects_non_finite_coordinate(self) -> None:
        payload = {
            "x": float("nan"),
            "y": 2.0,
            "z": 3.0,
            "heading": 0.0,
            "pitch": 0.0,
            "speed": 0.0,
            "direction": 0.0,
            "movementState": "idle",
            "jumping": False,
            "weaponDrawn": False,
            "cellId": 1,
            "worldspaceId": 0,
            "furniture": None,
            "graphEpoch": 1,
            "sequence": 1,
        }

        self.assertIsNone(parse_player_update(payload))

    def test_rejects_out_of_range_cell_id(self) -> None:
        payload = {
            "x": 1.0,
            "y": 2.0,
            "z": 3.0,
            "heading": 0.0,
            "pitch": 0.0,
            "speed": 125.0,
            "direction": 0.0,
            "movementState": "walking",
            "jumping": False,
            "weaponDrawn": False,
            "cellId": 0x1_0000_0000,
            "worldspaceId": 0,
            "furniture": None,
            "graphEpoch": 1,
            "sequence": 1,
        }

        self.assertIsNone(parse_player_update(payload))

    def test_rejects_unknown_movement_state(self) -> None:
        payload = {
            "x": 1.0,
            "y": 2.0,
            "z": 3.0,
            "heading": 0.0,
            "pitch": 0.0,
            "speed": 125.0,
            "direction": 0.0,
            "movementState": "flying",
            "jumping": False,
            "weaponDrawn": False,
            "cellId": 1,
            "worldspaceId": 0,
            "furniture": None,
            "graphEpoch": 1,
            "sequence": 1,
        }

        self.assertIsNone(parse_player_update(payload))

    def test_rejects_out_of_range_worldspace_id(self) -> None:
        payload = {
            "x": 1.0,
            "y": 2.0,
            "z": 3.0,
            "heading": 0.0,
            "pitch": 0.0,
            "speed": 0.0,
            "direction": 0.0,
            "movementState": "idle",
            "jumping": False,
            "weaponDrawn": False,
            "cellId": 1,
            "worldspaceId": 0x1_0000_0000,
            "furniture": None,
            "graphEpoch": 1,
            "sequence": 1,
        }

        self.assertIsNone(parse_player_update(payload))

    def test_rejects_malformed_furniture_reference(self) -> None:
        payload = {
            "x": 1.0,
            "y": 2.0,
            "z": 3.0,
            "heading": 0.0,
            "pitch": 0.0,
            "speed": 0.0,
            "direction": 0.0,
            "movementState": "idle",
            "jumping": False,
            "weaponDrawn": False,
            "cellId": 1,
            "worldspaceId": 0,
            "graphEpoch": 1,
            "sequence": 1,
        }

        for furniture in (
            0xA729,
            {"plugin": "", "formId": 0xA729},
            {"plugin": "Skyrim.esm", "formId": 0x100_0000},
            {"plugin": "Skyrim.esm", "formId": -1},
            {"plugin": "Skyrim.esm", "formId": True},
            {"plugin": "Skyrim.esm"},
            {"plugin": "x" * 261, "formId": 1},
        ):
            with self.subTest(furniture=furniture):
                self.assertIsNone(parse_player_update({**payload, "furniture": furniture}))

    def test_missing_furniture_means_none(self) -> None:
        payload = {
            "x": 1.0,
            "y": 2.0,
            "z": 3.0,
            "heading": 0.0,
            "pitch": 0.0,
            "speed": 0.0,
            "direction": 0.0,
            "movementState": "idle",
            "jumping": False,
            "weaponDrawn": False,
            "cellId": 1,
            "worldspaceId": 0,
            "furnitureRefId": 0xA729,
            "graphEpoch": 1,
            "sequence": 1,
        }

        parsed = parse_player_update(payload)

        self.assertIsNotNone(parsed)
        self.assertIsNone(parsed["furniture"])
        self.assertNotIn("furnitureRefId", parsed)

    def test_accepts_backward_direction(self) -> None:
        payload = {
            "x": 1.0,
            "y": 2.0,
            "z": 3.0,
            "heading": 0.0,
            "pitch": 0.0,
            "speed": 150.0,
            "direction": 0.5,
            "movementState": "walking",
            "jumping": False,
            "weaponDrawn": True,
            "bowDrawn": False,
            "castingLeft": False,
            "castingRight": False,
            "blocking": False,
            "dead": False,
            "mounted": False,
            "swimming": False,
            "cellId": 1,
            "worldspaceId": 0,
            "furniture": None,
            "graphEpoch": 1,
            "sequence": 1,
        }

        self.assertEqual(parse_player_update(payload), payload)

    def test_rejects_out_of_range_direction(self) -> None:
        for direction in (-0.25, 1.5, float("nan")):
            payload = {
                "x": 1.0,
                "y": 2.0,
                "z": 3.0,
                "heading": 0.0,
                "pitch": 0.0,
                "speed": 150.0,
                "direction": direction,
                "movementState": "walking",
                "jumping": False,
                "weaponDrawn": False,
                "cellId": 1,
                "worldspaceId": 0,
                "furniture": None,
                "graphEpoch": 1,
                "sequence": 1,
            }

            with self.subTest(direction=direction):
                self.assertIsNone(parse_player_update(payload))

    def test_rejects_negative_speed(self) -> None:
        payload = {
            "x": 1.0,
            "y": 2.0,
            "z": 3.0,
            "heading": 0.0,
            "pitch": 0.0,
            "speed": -1.0,
            "direction": 0.0,
            "movementState": "walking",
            "jumping": False,
            "weaponDrawn": False,
            "cellId": 1,
            "worldspaceId": 0,
            "furniture": None,
            "graphEpoch": 1,
            "sequence": 1,
        }

        self.assertIsNone(parse_player_update(payload))

    def test_defaults_missing_hand_presentation(self) -> None:
        payload = {
            "x": 1.0,
            "y": 2.0,
            "z": 3.0,
            "heading": 0.0,
            "pitch": 0.0,
            "speed": 0.0,
            "direction": 0.0,
            "movementState": "idle",
            "jumping": False,
            "weaponDrawn": True,
            "cellId": 1,
            "worldspaceId": 0,
            "furniture": None,
            "graphEpoch": 1,
            "sequence": 1,
        }

        self.assertEqual(
            parse_player_update(payload),
            {
                **payload,
                "bowDrawn": False,
                "castingLeft": False,
                "castingRight": False,
                "blocking": False,
                "dead": False,
                "mounted": False,
                "swimming": False,
            },
        )

    def test_accepts_dead_player(self) -> None:
        payload = {
            "x": 1.0,
            "y": 2.0,
            "z": 3.0,
            "heading": 0.0,
            "pitch": 0.0,
            "speed": 0.0,
            "direction": 0.0,
            "movementState": "idle",
            "jumping": False,
            "weaponDrawn": True,
            "bowDrawn": False,
            "castingLeft": False,
            "castingRight": False,
            "blocking": False,
            "dead": True,
            "mounted": False,
            "swimming": False,
            "cellId": 1,
            "worldspaceId": 0,
            "furniture": None,
            "graphEpoch": 1,
            "sequence": 1,
        }

        self.assertEqual(parse_player_update(payload), payload)

    def test_rejects_non_boolean_dead_state(self) -> None:
        for dead in (1, "true", None):
            payload = {
                "x": 1.0,
                "y": 2.0,
                "z": 3.0,
                "heading": 0.0,
                "pitch": 0.0,
                "speed": 0.0,
                "direction": 0.0,
                "movementState": "idle",
                "jumping": False,
                "weaponDrawn": False,
                "dead": dead,
                "cellId": 1,
                "worldspaceId": 0,
                "furniture": None,
                "graphEpoch": 1,
                "sequence": 1,
            }

            with self.subTest(dead=dead):
                self.assertIsNone(parse_player_update(payload))

    def test_accepts_mounted_player(self) -> None:
        payload = {
            "x": 1.0,
            "y": 2.0,
            "z": 3.0,
            "heading": 0.0,
            "pitch": 0.0,
            "speed": 700.0,
            "direction": 0.0,
            "movementState": "running",
            "jumping": False,
            "weaponDrawn": False,
            "bowDrawn": False,
            "castingLeft": False,
            "castingRight": False,
            "blocking": False,
            "dead": False,
            "mounted": True,
            "swimming": False,
            "cellId": 1,
            "worldspaceId": 60,
            "furniture": None,
            "graphEpoch": 1,
            "sequence": 1,
        }

        self.assertEqual(parse_player_update(payload), payload)

    def test_rejects_non_boolean_mounted_state(self) -> None:
        for mounted in (1, "true", None):
            payload = {
                "x": 1.0,
                "y": 2.0,
                "z": 3.0,
                "heading": 0.0,
                "pitch": 0.0,
                "speed": 0.0,
                "direction": 0.0,
                "movementState": "idle",
                "jumping": False,
                "weaponDrawn": False,
                "mounted": mounted,
                "cellId": 1,
                "worldspaceId": 60,
                "furniture": None,
                "graphEpoch": 1,
                "sequence": 1,
            }

            with self.subTest(mounted=mounted):
                self.assertIsNone(parse_player_update(payload))

    def test_accepts_pitched_mount(self) -> None:
        payload = {
            "x": 1.0,
            "y": 2.0,
            "z": 3.0,
            "heading": 0.0,
            "pitch": 0.35,
            "speed": 450.0,
            "direction": 0.0,
            "movementState": "running",
            "jumping": False,
            "weaponDrawn": False,
            "bowDrawn": False,
            "castingLeft": False,
            "castingRight": False,
            "blocking": False,
            "dead": False,
            "mounted": True,
            "swimming": False,
            "cellId": 1,
            "worldspaceId": 60,
            "furniture": None,
            "graphEpoch": 1,
            "sequence": 1,
        }

        self.assertEqual(parse_player_update(payload), payload)

    def test_defaults_missing_pitch_to_level(self) -> None:
        payload = {
            "x": 1.0,
            "y": 2.0,
            "z": 3.0,
            "heading": 0.0,
            "speed": 0.0,
            "direction": 0.0,
            "movementState": "idle",
            "jumping": False,
            "weaponDrawn": False,
            "cellId": 1,
            "worldspaceId": 60,
            "furniture": None,
            "graphEpoch": 1,
            "sequence": 1,
        }

        parsed = parse_player_update(payload)

        self.assertIsNotNone(parsed)
        assert parsed is not None
        self.assertEqual(parsed["pitch"], 0.0)

    def test_rejects_invalid_pitch(self) -> None:
        for pitch in (True, "0.1", None, float("nan"), float("inf"), 1.6, -1.6):
            payload = {
                "x": 1.0,
                "y": 2.0,
                "z": 3.0,
                "heading": 0.0,
                "pitch": pitch,
                "speed": 0.0,
                "direction": 0.0,
                "movementState": "idle",
                "jumping": False,
                "weaponDrawn": False,
                "cellId": 1,
                "worldspaceId": 60,
                "furniture": None,
                "graphEpoch": 1,
                "sequence": 1,
            }

            with self.subTest(pitch=pitch):
                self.assertIsNone(parse_player_update(payload))

    def test_accepts_swimming_player(self) -> None:
        payload = {
            "x": 1.0,
            "y": 2.0,
            "z": -40.0,
            "heading": 0.0,
            "pitch": 0.0,
            "speed": 150.0,
            "direction": 0.0,
            "movementState": "running",
            "jumping": False,
            "weaponDrawn": False,
            "bowDrawn": False,
            "castingLeft": False,
            "castingRight": False,
            "blocking": False,
            "dead": False,
            "mounted": False,
            "swimming": True,
            "cellId": 1,
            "worldspaceId": 60,
            "furniture": None,
            "graphEpoch": 1,
            "sequence": 1,
        }

        self.assertEqual(parse_player_update(payload), payload)

    def test_rejects_non_boolean_swimming_state(self) -> None:
        for swimming in (1, "true", None):
            payload = {
                "x": 1.0,
                "y": 2.0,
                "z": 3.0,
                "heading": 0.0,
                "pitch": 0.0,
                "speed": 0.0,
                "direction": 0.0,
                "movementState": "idle",
                "jumping": False,
                "weaponDrawn": False,
                "swimming": swimming,
                "cellId": 1,
                "worldspaceId": 60,
                "furniture": None,
                "graphEpoch": 1,
                "sequence": 1,
            }

            with self.subTest(swimming=swimming):
                self.assertIsNone(parse_player_update(payload))

    def test_rejects_non_boolean_hand_presentation(self) -> None:
        for field in ("bowDrawn", "castingLeft", "castingRight", "blocking"):
            payload = {
                "x": 1.0,
                "y": 2.0,
                "z": 3.0,
                "heading": 0.0,
                "pitch": 0.0,
                "speed": 0.0,
                "direction": 0.0,
                "movementState": "idle",
                "jumping": False,
                "weaponDrawn": True,
                field: 1,
                "cellId": 1,
                "worldspaceId": 0,
                "furniture": None,
                "graphEpoch": 1,
                "sequence": 1,
            }

            with self.subTest(field=field):
                self.assertIsNone(parse_player_update(payload))


class ParseAnimationActionTests(unittest.TestCase):
    def test_accepts_normal_attack(self) -> None:
        payload = {
            "actionSequence": 3,
            "graphEpoch": 2,
            "checkpointSequence": 18,
            "family": "attack",
            "action": "normal",
            "power": False,
            "prerequisites": {"weaponDrawn": True},
        }

        self.assertEqual(parse_animation_action(payload), payload)

    def test_accepts_off_hand_attacks_and_bash(self) -> None:
        for action in ("left", "dual", "bash"):
            payload = {
                "actionSequence": 3,
                "graphEpoch": 2,
                "checkpointSequence": 18,
                "family": "attack",
                "action": action,
                "power": False,
                "prerequisites": {"weaponDrawn": True},
            }

            with self.subTest(action=action):
                self.assertEqual(parse_animation_action(payload), payload)

    def test_accepts_power_attacks_with_direction(self) -> None:
        for action in ("normal", "left", "dual", "bash"):
            for direction in ("standing", "forward", "backward", "left", "right"):
                payload = {
                    "actionSequence": 3,
                    "graphEpoch": 2,
                    "checkpointSequence": 18,
                    "family": "attack",
                    "action": action,
                    "power": True,
                    "direction": direction,
                    "prerequisites": {"weaponDrawn": True},
                }

                with self.subTest(action=action, direction=direction):
                    self.assertEqual(parse_animation_action(payload), payload)

    def test_rejects_invalid_power_fields(self) -> None:
        base = {
            "actionSequence": 3,
            "graphEpoch": 2,
            "checkpointSequence": 18,
            "family": "attack",
            "action": "normal",
            "prerequisites": {"weaponDrawn": True},
        }
        invalid = (
            {},
            {"power": 1},
            {"power": None},
            {"power": True},
            {"power": True, "direction": "sprint"},
            {"power": True, "direction": "attackPowerStartForward"},
            {"power": True, "direction": None},
            {"power": False, "direction": "forward"},
            {"power": False, "direction": None},
            {"direction": "standing"},
        )

        for fields in invalid:
            with self.subTest(fields=fields):
                self.assertIsNone(parse_animation_action({**base, **fields}))

    def test_rejects_arbitrary_graph_event(self) -> None:
        payload = {
            "actionSequence": 3,
            "graphEpoch": 2,
            "checkpointSequence": 18,
            "family": "attack",
            "action": "attackPowerStart",
            "power": False,
            "prerequisites": {"weaponDrawn": True},
        }

        self.assertIsNone(parse_animation_action(payload))

    def test_rejects_attack_without_drawn_prerequisite(self) -> None:
        payload = {
            "actionSequence": 3,
            "graphEpoch": 2,
            "checkpointSequence": 18,
            "family": "attack",
            "action": "normal",
            "power": False,
            "prerequisites": {"weaponDrawn": False},
        }

        self.assertIsNone(parse_animation_action(payload))


if __name__ == "__main__":
    unittest.main()


class ParseChatMessageTests(unittest.TestCase):
    def test_accepts_text_and_trims_surrounding_spaces(self) -> None:
        self.assertEqual(parse_chat_message({"text": "  Hello, Whiterun  "}), {"text": "Hello, Whiterun"})
        self.assertEqual(parse_chat_message({"text": "Привіт"}), {"text": "Привіт"})

    def test_accepts_text_up_to_the_byte_limit(self) -> None:
        # Two UTF-8 bytes per character.
        text = "é" * (MAXIMUM_CHAT_TEXT_BYTES // 2)
        self.assertEqual(parse_chat_message({"text": text}), {"text": text})
        self.assertIsNone(parse_chat_message({"text": text + "é"}))

    def test_rejects_invalid_text(self) -> None:
        for text in (None, 7, True, "", "   ", "line\nbreak", "tab\there", "bell\x07", "del\x7f", "\ud800"):
            with self.subTest(text=text):
                self.assertIsNone(parse_chat_message({"text": text}))

        self.assertIsNone(parse_chat_message(None))
        self.assertIsNone(parse_chat_message("hello"))
        self.assertIsNone(parse_chat_message({}))
