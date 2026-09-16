import json
import tempfile
import unittest
from pathlib import Path
from unittest.mock import AsyncMock, patch

import server.app as server_app
from server.protocol import parse_player_appearance
from server.replay import load_recording


def valid_appearance(revision: int = 1) -> dict[str, object]:
    return {
        "revision": revision,
        "name": "Lydia Stormblade",
        "race": {"plugin": "Skyrim.esm", "formId": 0x13746},
        "female": True,
        "weight": 42.5,
        "height": 1.0,
        "headParts": [
            {"plugin": "Skyrim.esm", "formId": 0x51575, "type": 1},
            {"plugin": "KS Hairdos.esp", "formId": 0x812, "type": 3},
        ],
        "hairColor": {"r": 59, "g": 42, "b": 30},
        "faceDetails": None,
        "bodyTint": {"r": 222, "g": 188, "b": 170},
        "faceMorphs": [0.0] * 18 + [3.4028234663852886e38],
        "faceParts": [0, 1, -1, 2],
        "tints": [{"index": 44, "type": 6, "r": 222, "g": 188, "b": 170, "alpha": 1.0}],
    }


class ParsePlayerAppearanceTests(unittest.TestCase):
    def test_accepts_valid_appearance(self) -> None:
        self.assertEqual(parse_player_appearance(valid_appearance()), valid_appearance())

    def test_accepts_null_hair_color_and_face_details_reference(self) -> None:
        payload = valid_appearance()
        payload["hairColor"] = None
        payload["faceDetails"] = {"plugin": "Skyrim.esm", "formId": 0x1234}

        self.assertEqual(parse_player_appearance(payload), payload)

    def test_accepts_empty_and_multibyte_names(self) -> None:
        for name in ("", "Олександра", "é" * 64):
            with self.subTest(name):
                payload = valid_appearance()
                payload["name"] = name
                self.assertEqual(parse_player_appearance(payload), payload)

    def test_missing_or_null_name_is_empty(self) -> None:
        expected = valid_appearance()
        expected["name"] = ""
        missing = valid_appearance()
        missing.pop("name")
        null = valid_appearance()
        null["name"] = None

        self.assertEqual(parse_player_appearance(missing), expected)
        self.assertEqual(parse_player_appearance(null), expected)

    def test_drops_unknown_fields(self) -> None:
        payload = valid_appearance()
        payload["extra"] = True
        payload["race"] = {"plugin": "Skyrim.esm", "formId": 0x13746, "extra": 1}

        self.assertEqual(parse_player_appearance(payload), valid_appearance())

    def test_rejects_invalid_values(self) -> None:
        mutations = {
            "zero revision": lambda p: p.update(revision=0),
            "non-string name": lambda p: p.update(name=7),
            "name with control character": lambda p: p.update(name="Lydia\nStormblade"),
            "name too long": lambda p: p.update(name="é" * 65),
            "name with lone surrogate": lambda p: p.update(name="\ud800"),
            "boolean weight": lambda p: p.update(weight=True),
            "weight too large": lambda p: p.update(weight=101.0),
            "height too small": lambda p: p.update(height=0.1),
            "non-finite morph": lambda p: p["faceMorphs"].__setitem__(0, float("nan")),
            "missing morph": lambda p: p["faceMorphs"].pop(),
            "face part overflow": lambda p: p["faceParts"].__setitem__(0, 2**31),
            "empty plugin": lambda p: p["race"].update(plugin=""),
            "form id too large": lambda p: p["race"].update(formId=0x1000000),
            "head part type": lambda p: p["headParts"][0].update(type=7),
            "too many head parts": lambda p: p.update(headParts=p["headParts"] * 9),
            "missing hair color": lambda p: p.pop("hairColor"),
            "color channel": lambda p: p["bodyTint"].update(r=256),
            "tint alpha": lambda p: p["tints"][0].update(alpha=1.5),
            "tint type": lambda p: p["tints"][0].update(type=15),
            "too many tints": lambda p: p.update(tints=p["tints"] * 65),
        }
        for name, mutate in mutations.items():
            with self.subTest(name):
                payload = valid_appearance()
                mutate(payload)
                self.assertIsNone(parse_player_appearance(payload))


class PlayerAppearanceRelayTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self) -> None:
        server_app.player_appearances.clear()

    def tearDown(self) -> None:
        server_app.player_appearances.clear()

    async def test_relays_and_stores_appearance(self) -> None:
        emit = AsyncMock()

        with patch.object(server_app.sio, "emit", new=emit):
            result = await server_app.player_appearance("player-1", valid_appearance())

        self.assertEqual(result, {"ok": True})
        self.assertEqual(server_app.player_appearances["player-1"], valid_appearance())
        emit.assert_awaited_once_with(
            "player:appearance",
            {"playerId": "player-1", **valid_appearance()},
            skip_sid="player-1",
        )

    async def test_rejects_stale_revision(self) -> None:
        with patch.object(server_app.sio, "emit", new=AsyncMock()):
            await server_app.player_appearance("player-1", valid_appearance(2))
            result = await server_app.player_appearance("player-1", valid_appearance(2))

        self.assertEqual(result, {"ok": False, "error": "stale appearance revision"})

    async def test_rejects_malformed_appearance(self) -> None:
        result = await server_app.player_appearance("player-1", {"revision": 1})

        self.assertEqual(result, {"ok": False, "error": "invalid player appearance"})

    async def test_sends_stored_appearances_to_late_joiner(self) -> None:
        server_app.player_appearances["player-1"] = valid_appearance(3)
        emit = AsyncMock()

        with patch.object(server_app.sio, "emit", new=emit):
            await server_app.connect("player-2", {})

        emit.assert_awaited_once_with(
            "player:appearance",
            {"playerId": "player-1", **valid_appearance(3)},
            to="player-2",
        )

    async def test_forgets_appearance_on_disconnect(self) -> None:
        server_app.player_appearances["player-1"] = valid_appearance()

        with patch.object(server_app.sio, "emit", new=AsyncMock()):
            await server_app.disconnect("player-1", "client disconnect")

        self.assertNotIn("player-1", server_app.player_appearances)


class PlayerAppearanceRecordingTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addAsyncCleanup(self.temporary_directory.cleanup)
        self.recordings_directory = Path(self.temporary_directory.name) / "recordings"
        self.original_recordings_directory = server_app.RECORDINGS_DIRECTORY
        self.original_recording_enabled = server_app.RECORDING_ENABLED
        server_app.RECORDINGS_DIRECTORY = self.recordings_directory
        server_app.RECORDING_ENABLED = True
        server_app.player_appearances.clear()
        server_app.player_recordings.clear()

    async def asyncTearDown(self) -> None:
        server_app.RECORDINGS_DIRECTORY = self.original_recordings_directory
        server_app.RECORDING_ENABLED = self.original_recording_enabled
        server_app.player_appearances.clear()
        server_app.player_recordings.clear()

    async def test_records_appearance_that_replay_can_load(self) -> None:
        with patch.object(server_app.sio, "emit", new=AsyncMock()):
            await server_app.player_appearance("player-1", valid_appearance())

        path = self.recordings_directory / "player-1.json"
        document = json.loads(path.read_text(encoding="utf-8"))
        self.assertEqual(document["messages"][0]["event"], "player:appearance")

        updates = load_recording(path)
        self.assertEqual(len(updates), 1)
        self.assertEqual(updates[0].event, "player:appearance")
        self.assertEqual(updates[0].payload, valid_appearance())
