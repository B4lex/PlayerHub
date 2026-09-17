import unittest

from server.protocol import parse_animation_action, parse_player_update


class ParsePlayerUpdateTests(unittest.TestCase):
    def test_accepts_valid_transform(self) -> None:
        payload = {
            "x": 1.0,
            "y": 2,
            "z": -3.5,
            "heading": 0.25,
            "speed": 550.0,
            "direction": 0.0,
            "movementState": "sprinting",
            "jumping": False,
            "weaponDrawn": True,
            "bowDrawn": True,
            "castingLeft": False,
            "castingRight": True,
            "blocking": True,
            "cellId": 0x1234,
            "worldspaceId": 0x3C,
            "furnitureRefId": 0xA729,
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
            "speed": 200.0,
            "direction": 0.0,
            "movementState": "running",
            "jumping": True,
            "weaponDrawn": False,
            "bowDrawn": False,
            "castingLeft": False,
            "castingRight": False,
            "blocking": False,
            "cellId": 0x1234,
            "worldspaceId": 0x3C,
            "furnitureRefId": 0,
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
            "speed": 200.0,
            "direction": 0.0,
            "movementState": "running",
            "jumping": 1,
            "weaponDrawn": False,
            "cellId": 0x1234,
            "worldspaceId": 0x3C,
            "furnitureRefId": 0,
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
            "speed": 0.0,
            "direction": 0.0,
            "movementState": "idle",
            "jumping": False,
            "weaponDrawn": 1,
            "cellId": 0x1234,
            "worldspaceId": 0x3C,
            "furnitureRefId": 0,
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
            "speed": 0.0,
            "direction": 0.0,
            "movementState": "idle",
            "jumping": False,
            "weaponDrawn": False,
            "cellId": 1,
            "worldspaceId": 0,
            "furnitureRefId": 0,
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
            "speed": 125.0,
            "direction": 0.0,
            "movementState": "walking",
            "jumping": False,
            "weaponDrawn": False,
            "cellId": 0x1_0000_0000,
            "worldspaceId": 0,
            "furnitureRefId": 0,
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
            "speed": 125.0,
            "direction": 0.0,
            "movementState": "flying",
            "jumping": False,
            "weaponDrawn": False,
            "cellId": 1,
            "worldspaceId": 0,
            "furnitureRefId": 0,
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
            "speed": 0.0,
            "direction": 0.0,
            "movementState": "idle",
            "jumping": False,
            "weaponDrawn": False,
            "cellId": 1,
            "worldspaceId": 0x1_0000_0000,
            "furnitureRefId": 0,
            "graphEpoch": 1,
            "sequence": 1,
        }

        self.assertIsNone(parse_player_update(payload))

    def test_rejects_out_of_range_furniture_reference_id(self) -> None:
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
            "worldspaceId": 0,
            "furnitureRefId": 0x1_0000_0000,
            "graphEpoch": 1,
            "sequence": 1,
        }

        self.assertIsNone(parse_player_update(payload))

    def test_rejects_missing_furniture_reference_id(self) -> None:
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
            "worldspaceId": 0,
            "graphEpoch": 1,
            "sequence": 1,
        }

        self.assertIsNone(parse_player_update(payload))

    def test_accepts_backward_direction(self) -> None:
        payload = {
            "x": 1.0,
            "y": 2.0,
            "z": 3.0,
            "heading": 0.0,
            "speed": 150.0,
            "direction": 0.5,
            "movementState": "walking",
            "jumping": False,
            "weaponDrawn": True,
            "bowDrawn": False,
            "castingLeft": False,
            "castingRight": False,
            "blocking": False,
            "cellId": 1,
            "worldspaceId": 0,
            "furnitureRefId": 0,
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
                "speed": 150.0,
                "direction": direction,
                "movementState": "walking",
                "jumping": False,
                "weaponDrawn": False,
                "cellId": 1,
                "worldspaceId": 0,
                "furnitureRefId": 0,
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
            "speed": -1.0,
            "direction": 0.0,
            "movementState": "walking",
            "jumping": False,
            "weaponDrawn": False,
            "cellId": 1,
            "worldspaceId": 0,
            "furnitureRefId": 0,
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
            "speed": 0.0,
            "direction": 0.0,
            "movementState": "idle",
            "jumping": False,
            "weaponDrawn": True,
            "cellId": 1,
            "worldspaceId": 0,
            "furnitureRefId": 0,
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
            },
        )

    def test_rejects_non_boolean_hand_presentation(self) -> None:
        for field in ("bowDrawn", "castingLeft", "castingRight", "blocking"):
            payload = {
                "x": 1.0,
                "y": 2.0,
                "z": 3.0,
                "heading": 0.0,
                "speed": 0.0,
                "direction": 0.0,
                "movementState": "idle",
                "jumping": False,
                "weaponDrawn": True,
                field: 1,
                "cellId": 1,
                "worldspaceId": 0,
                "furnitureRefId": 0,
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
                "prerequisites": {"weaponDrawn": True},
            }

            with self.subTest(action=action):
                self.assertEqual(parse_animation_action(payload), payload)

    def test_rejects_arbitrary_graph_event(self) -> None:
        payload = {
            "actionSequence": 3,
            "graphEpoch": 2,
            "checkpointSequence": 18,
            "family": "attack",
            "action": "attackPowerStart",
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
            "prerequisites": {"weaponDrawn": False},
        }

        self.assertIsNone(parse_animation_action(payload))


if __name__ == "__main__":
    unittest.main()
