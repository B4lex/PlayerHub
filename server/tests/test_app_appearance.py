import tempfile
import unittest
from pathlib import Path
from unittest.mock import AsyncMock, patch

import server.app as server_app
from server.protocol import parse_player_appearance


def valid_appearance(revision: int = 1) -> dict[str, object]:
    return {
        "revision": revision,
        "name": "Lydia Stormblade",
        "level": 42,
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
        "defaultRace": None,
    }


def modded_race_appearance() -> dict[str, object]:
    payload = valid_appearance()
    payload["race"] = {"plugin": "Lykaios Race.esp", "formId": 0xD62}
    payload["defaultRace"] = {"race": {"plugin": "Skyrim.esm", "formId": 0x13745}, "female": False}
    return payload


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

    def test_missing_or_null_level_is_unknown(self) -> None:
        expected = valid_appearance()
        expected["level"] = 0
        missing = valid_appearance()
        missing.pop("level")
        null = valid_appearance()
        null["level"] = None

        self.assertEqual(parse_player_appearance(missing), expected)
        self.assertEqual(parse_player_appearance(null), expected)
        self.assertEqual(parse_player_appearance(expected), expected)

    def test_accepts_base_game_default_race(self) -> None:
        payload = modded_race_appearance()
        self.assertEqual(parse_player_appearance(payload), payload)

        payload["defaultRace"]["race"]["plugin"] = "SKYRIM.ESM"
        self.assertEqual(parse_player_appearance(payload), payload)

    def test_missing_default_race_is_none(self) -> None:
        payload = valid_appearance()
        payload.pop("defaultRace")

        self.assertEqual(parse_player_appearance(payload), valid_appearance())

    def test_rejects_invalid_default_race(self) -> None:
        mutations = {
            "not an object": lambda p: p.update(defaultRace=True),
            "missing race": lambda p: p["defaultRace"].pop("race"),
            "null race": lambda p: p["defaultRace"].update(race=None),
            "modded race": lambda p: p["defaultRace"]["race"].update(plugin="Lykaios Race.esp"),
            "missing female": lambda p: p["defaultRace"].pop("female"),
            "non-boolean female": lambda p: p["defaultRace"].update(female=1),
        }
        for name, mutate in mutations.items():
            with self.subTest(name):
                payload = modded_race_appearance()
                mutate(payload)
                self.assertIsNone(parse_player_appearance(payload))

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
            "boolean level": lambda p: p.update(level=True),
            "negative level": lambda p: p.update(level=-1),
            "level too large": lambda p: p.update(level=0x10000),
            "fractional level": lambda p: p.update(level=1.5),
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


def clear_appearances() -> None:
    server_app.player_appearances.clear()
    server_app.registry.name_indices.clear()


class PlayerAppearanceRelayTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self) -> None:
        clear_appearances()

    def tearDown(self) -> None:
        clear_appearances()

    async def test_relays_and_stores_appearance(self) -> None:
        emit = AsyncMock()

        with patch.object(server_app.sio, "emit", new=emit):
            result = await server_app.player_appearance("player-1", valid_appearance())

        self.assertEqual(result, {"ok": True})
        self.assertEqual(server_app.player_appearances["player-1"], valid_appearance())
        emit.assert_awaited_once_with(
            "player:appearance",
            {"playerId": "player-1", **valid_appearance(), "nameIndex": 1},
            skip_sid="player-1",
        )

    async def test_indexes_players_with_the_same_name(self) -> None:
        emit = AsyncMock()

        async def publish(player_id: str, name: str, revision: int = 1) -> int:
            payload = valid_appearance(revision)
            payload["name"] = name
            with patch.object(server_app.sio, "emit", new=emit):
                self.assertEqual(await server_app.player_appearance(player_id, payload), {"ok": True})
            return emit.await_args.args[1]["nameIndex"]

        self.assertEqual(await publish("player-1", "Lydia"), 1)
        self.assertEqual(await publish("player-2", "lydia"), 2)
        self.assertEqual(await publish("player-3", "Aela"), 1)
        # A new revision under the same name keeps the index.
        self.assertEqual(await publish("player-2", "Lydia", 2), 2)
        with patch.object(server_app.sio, "emit", new=AsyncMock()):
            await server_app.disconnect("player-1", "client disconnect")
        # Indexes are not renumbered; a newcomer takes the lowest free one.
        self.assertEqual(await publish("player-4", "Lydia"), 1)
        self.assertEqual(await publish("player-3", "Lydia", 2), 3)
        self.assertEqual(await publish("player-4", "Serana", 2), 1)
        self.assertEqual(await publish("player-5", "Lydia"), 1)

    async def test_rejects_stale_revision(self) -> None:
        with patch.object(server_app.sio, "emit", new=AsyncMock()):
            await server_app.player_appearance("player-1", valid_appearance(2))
            result = await server_app.player_appearance("player-1", valid_appearance(2))

        self.assertEqual(result, {"ok": False, "error": "stale appearance revision"})

    async def test_rejects_malformed_appearance(self) -> None:
        result = await server_app.player_appearance("player-1", {"revision": 1})

        self.assertEqual(result, {"ok": False, "error": "invalid player appearance"})

    async def test_sends_stored_appearances_to_late_joiner(self) -> None:
        server_app.registry.set_appearance("player-1", valid_appearance(3))
        server_app.registry.set_appearance("player-3", valid_appearance(1))
        emit = AsyncMock()

        with patch.object(server_app.sio, "emit", new=emit):
            await server_app.connect("player-2", {})

        emit.assert_any_await(
            "player:appearance",
            {"playerId": "player-1", **valid_appearance(3), "nameIndex": 1},
            to="player-2",
        )
        emit.assert_any_await(
            "player:appearance",
            {"playerId": "player-3", **valid_appearance(1), "nameIndex": 2},
            to="player-2",
        )

    async def test_forgets_appearance_on_disconnect(self) -> None:
        server_app.registry.set_appearance("player-1", valid_appearance())

        with patch.object(server_app.sio, "emit", new=AsyncMock()):
            await server_app.disconnect("player-1", "client disconnect")

        self.assertNotIn("player-1", server_app.player_appearances)
        self.assertNotIn("player-1", server_app.registry.name_indices)


class PlayerAppearanceCaptureTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addAsyncCleanup(self.temporary_directory.cleanup)
        self.original_directory = server_app.recording_store.directory
        server_app.recording_store.directory = Path(self.temporary_directory.name) / "recordings"
        clear_appearances()

    async def asyncTearDown(self) -> None:
        server_app.captures.stop_all("test cleanup")
        server_app.recording_store.directory = self.original_directory
        clear_appearances()

    async def test_captures_appearance_of_a_recorded_player_only(self) -> None:
        info = server_app.captures.start("player-1", "", "", [])

        with patch.object(server_app.sio, "emit", new=AsyncMock()):
            await server_app.player_appearance("player-1", valid_appearance())
            await server_app.player_appearance("player-2", valid_appearance())
        server_app.captures.stop(info.id)

        recording = server_app.recording_store.load(info.id)
        self.assertEqual([m.event for m in recording.messages], ["player:appearance"])
        self.assertEqual(recording.messages[0].payload, valid_appearance())
