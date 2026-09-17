import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, call, patch

from server.replay import RecordedUpdate, load_recording, replay


def valid_payload(sequence: int = 1) -> dict[str, bool | int | float | str]:
    return {
        "x": 1.0,
        "y": 2.0,
        "z": 3.0,
        "heading": 0.0,
        "speed": 100.0,
        "direction": 0.0,
        "movementState": "idle",
        "jumping": False,
        "weaponDrawn": False,
        "cellId": 1,
        "worldspaceId": 0,
        "furnitureRefId": 0,
        "graphEpoch": 1,
        "sequence": sequence,
    }


def valid_action(sequence: int = 1) -> dict[str, object]:
    return {
        "actionSequence": sequence,
        "graphEpoch": 1,
        "checkpointSequence": 1,
        "family": "attack",
        "action": "normal",
        "power": False,
        "prerequisites": {"weaponDrawn": True},
    }


class LoadRecordingTests(unittest.TestCase):
    def write_recording(self, contents: object) -> Path:
        file = tempfile.NamedTemporaryFile(mode="w", suffix=".json", delete=False, encoding="utf-8")
        with file:
            json.dump(contents, file)
        self.addCleanup(Path(file.name).unlink)
        return Path(file.name)

    def test_uses_default_and_explicit_delays(self) -> None:
        path = self.write_recording(
            {
                "defaultDelayMs": 50,
                "messages": [valid_payload(), {"delayMs": 100, "payload": valid_payload(2)}],
            }
        )

        updates = load_recording(path)

        self.assertEqual([update.delay_seconds for update in updates], [0.05, 0.1])
        self.assertEqual(updates[1].payload["sequence"], 2)

    def test_derives_delays_from_timestamps(self) -> None:
        path = self.write_recording(
            [
                {"timestampMs": 120, "payload": valid_payload()},
                {"timestampMs": 170, "payload": valid_payload(2)},
            ]
        )

        updates = load_recording(path)

        self.assertEqual([update.delay_seconds for update in updates], [0.0, 0.05])

    def test_loads_interleaved_animation_actions(self) -> None:
        checkpoint = valid_payload()
        checkpoint["weaponDrawn"] = True
        path = self.write_recording(
            {
                "messages": [
                    {
                        "timestampMs": 100,
                        "event": "player:update",
                        "payload": checkpoint,
                    },
                    {
                        "timestampMs": 125,
                        "event": "animation:action",
                        "payload": valid_action(),
                    },
                ]
            }
        )

        messages = load_recording(path)

        self.assertEqual([message.event for message in messages], ["player:update", "animation:action"])
        self.assertEqual([message.delay_seconds for message in messages], [0.0, 0.025])
        self.assertEqual(messages[1].payload, valid_action())

    def test_rejects_invalid_animation_action(self) -> None:
        path = self.write_recording(
            {
                "messages": [
                    {
                        "event": "animation:action",
                        "payload": {**valid_action(), "action": "arbitrary-event"},
                    }
                ]
            }
        )

        with self.assertRaisesRegex(ValueError, "invalid animation:action payload"):
            load_recording(path)

    @patch("server.replay.socketio.Client")
    def test_replays_updates_and_actions_through_their_protocol_events(
        self, client_factory: Mock
    ) -> None:
        client = client_factory.return_value
        client.call.return_value = {"ok": True}
        checkpoint = valid_payload()
        checkpoint["weaponDrawn"] = True
        messages = [
            RecordedUpdate(checkpoint, 0.0, "player:update"),
            RecordedUpdate(valid_action(), 0.0, "animation:action"),
        ]

        replay(messages, "http://localhost:3000", 1.0, False, 0.0, True, 5.0)

        self.assertEqual(
            client.call.call_args_list,
            [
                call("player:update", checkpoint, timeout=5.0),
                call("animation:action", valid_action(), timeout=5.0),
            ],
        )
        client.disconnect.assert_called_once_with()

    def test_rejects_descending_timestamps(self) -> None:
        path = self.write_recording(
            [
                {"timestampMs": 120, "payload": valid_payload()},
                {"timestampMs": 100, "payload": valid_payload(2)},
            ]
        )

        with self.assertRaisesRegex(ValueError, "must not go backwards"):
            load_recording(path)

    def test_rejects_non_finite_timing(self) -> None:
        path = self.write_recording([{"delayMs": float("nan"), "payload": valid_payload()}])

        with self.assertRaisesRegex(ValueError, "non-negative number"):
            load_recording(path)

    def test_infers_speed_for_legacy_recording(self) -> None:
        first = valid_payload()
        second = valid_payload(2)
        first.pop("speed")
        second.pop("speed")
        first.pop("jumping")
        second.pop("jumping")
        first.pop("weaponDrawn")
        second.pop("weaponDrawn")
        first.pop("graphEpoch")
        second.pop("graphEpoch")
        first.pop("furnitureRefId")
        second.pop("furnitureRefId")
        first.pop("direction")
        second.pop("direction")
        second["x"] = 21.0
        path = self.write_recording(
            [
                {"timestampMs": 100, "payload": first},
                {"timestampMs": 150, "payload": second},
            ]
        )

        updates = load_recording(path)

        self.assertEqual(updates[0].payload["speed"], 0.0)
        self.assertEqual(updates[1].payload["speed"], 400.0)
        self.assertIs(updates[1].payload["jumping"], False)
        self.assertIs(updates[1].payload["weaponDrawn"], False)
        self.assertEqual(updates[1].payload["graphEpoch"], 1)
        self.assertEqual(updates[1].payload["furnitureRefId"], 0)
        self.assertEqual(updates[1].payload["direction"], 0.0)
