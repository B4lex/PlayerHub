import tempfile
import unittest
from pathlib import Path
from unittest.mock import AsyncMock, call, patch

import server.app as server_app
from server.protocol import parse_player_equipment


def valid_equipment(revision: int = 1) -> dict[str, object]:
    return {
        "revision": revision,
        "armor": [
            {"plugin": "Skyrim.esm", "formId": 0x12E49, "slotMask": 4},
            {"plugin": "Dawnguard.esm", "formId": 0x13E7, "slotMask": 512},
            {"plugin": "Some Mod.esp", "formId": 0x812, "slotMask": 1 << 31 | 1 << 14},
        ],
        "rightHand": {"plugin": "Skyrim.esm", "formId": 0x12EB7, "kind": "weapon", "weaponType": 1},
        "leftHand": {"plugin": "Skyrim.esm", "formId": 0x1D4EC, "kind": "torch"},
        "ammo": {"plugin": "Skyrim.esm", "formId": 0x1397D},
        "defaultOutfit": {
            "armor": [
                {"plugin": "Skyrim.esm", "formId": 0x13911, "slotMask": 4},
                {"plugin": "Skyrim.esm", "formId": 0x13913, "slotMask": 3},
            ],
            "hands": [
                {"plugin": "Skyrim.esm", "formId": 0x1359D, "kind": "weapon", "weaponType": 5},
                {"plugin": "Skyrim.esm", "formId": 0x1D4EC, "kind": "torch"},
            ],
            "ammo": {"plugin": "Dawnguard.esm", "formId": 0xBB3},
        },
    }


class ParsePlayerEquipmentTests(unittest.TestCase):
    def test_accepts_valid_equipment(self) -> None:
        self.assertEqual(parse_player_equipment(valid_equipment()), valid_equipment())

    def test_accepts_empty_equipment(self) -> None:
        payload = {
            "revision": 1,
            "armor": [],
            "rightHand": None,
            "leftHand": None,
            "ammo": None,
            "defaultOutfit": {"armor": [], "hands": [], "ammo": None},
        }

        self.assertEqual(parse_player_equipment(payload), payload)

    def test_accepts_two_handed_weapon_with_empty_left_hand(self) -> None:
        payload = valid_equipment()
        payload["rightHand"] = {"plugin": "Skyrim.esm", "formId": 0x1397E, "kind": "weapon", "weaponType": 7}
        payload["leftHand"] = None

        self.assertEqual(parse_player_equipment(payload), payload)

    def test_accepts_dual_wielded_same_weapon(self) -> None:
        payload = valid_equipment()
        payload["leftHand"] = dict(payload["rightHand"])

        self.assertEqual(parse_player_equipment(payload), payload)

    def test_accepts_spells_in_both_hands_without_identity(self) -> None:
        payload = valid_equipment()
        payload["rightHand"] = {"kind": "spell", "plugin": "Skyrim.esm", "formId": 0x2DD2A}
        payload["leftHand"] = {"kind": "spell"}

        expected = valid_equipment()
        expected["rightHand"] = {"kind": "spell"}
        expected["leftHand"] = {"kind": "spell"}
        self.assertEqual(parse_player_equipment(payload), expected)

    def test_defaults_missing_ammo(self) -> None:
        payload = valid_equipment()
        del payload["ammo"]

        self.assertEqual(parse_player_equipment(payload), {**payload, "ammo": None})

    def test_defaults_missing_default_outfit(self) -> None:
        payload = valid_equipment()
        del payload["defaultOutfit"]

        self.assertEqual(
            parse_player_equipment(payload),
            {**payload, "defaultOutfit": {"armor": [], "hands": [], "ammo": None}},
        )

    def test_accepts_base_game_plugin_names_in_any_case(self) -> None:
        payload = valid_equipment()
        payload["defaultOutfit"]["armor"][0]["plugin"] = "SKYRIM.ESM"
        payload["defaultOutfit"]["ammo"]["plugin"] = "dawnguard.esm"

        self.assertEqual(parse_player_equipment(payload), payload)

    def test_accepts_anniversary_edition_backpack(self) -> None:
        payload = valid_equipment()
        payload["defaultOutfit"]["armor"].append(
            {"plugin": "ccFSVSSE001-Backpacks.esl", "formId": 0x801, "slotMask": 1 << 17}
        )

        self.assertEqual(parse_player_equipment(payload), payload)

    def test_drops_unknown_fields(self) -> None:
        payload = valid_equipment()
        payload["extra"] = True
        payload["armor"][0]["extra"] = 1
        payload["leftHand"]["weaponType"] = 3
        payload["defaultOutfit"]["extra"] = True
        payload["defaultOutfit"]["hands"][1]["weaponType"] = 3

        self.assertEqual(parse_player_equipment(payload), valid_equipment())

    def test_rejects_invalid_values(self) -> None:
        mutations = {
            "zero revision": lambda p: p.update(revision=0),
            "armor not a list": lambda p: p.update(armor={}),
            "too many armor entries": lambda p: p.update(
                armor=[
                    {"plugin": "Skyrim.esm", "formId": i, "slotMask": 1}
                    for i in range(33)
                ]
            ),
            "duplicate armor": lambda p: p["armor"].append(dict(p["armor"][0])),
            "zero slot mask": lambda p: p["armor"][0].update(slotMask=0),
            "slot mask overflow": lambda p: p["armor"][0].update(slotMask=2**32),
            "empty plugin": lambda p: p["armor"][0].update(plugin=""),
            "form id too large": lambda p: p["rightHand"].update(formId=0x1000000),
            "missing right hand": lambda p: p.pop("rightHand"),
            "missing left hand": lambda p: p.pop("leftHand"),
            "hand kind": lambda p: p["rightHand"].update(kind="staff"),
            "spell hand not an object": lambda p: p.update(leftHand="spell"),
            "ammo without form id": lambda p: p["ammo"].pop("formId"),
            "ammo not an object": lambda p: p.update(ammo=0x1397D),
            "hand-to-hand weapon": lambda p: p["rightHand"].update(weaponType=0),
            "weapon type too large": lambda p: p["rightHand"].update(weaponType=10),
            "missing weapon type": lambda p: p["rightHand"].pop("weaponType"),
            "torch in right hand": lambda p: p.update(rightHand=dict(p["leftHand"])),
            "two-handed in left hand": lambda p: p.update(
                leftHand={"plugin": "Skyrim.esm", "formId": 1, "kind": "weapon", "weaponType": 5}
            ),
            "two-handed with left hand": lambda p: p["rightHand"].update(weaponType=6),
            "default outfit not an object": lambda p: p.update(defaultOutfit=[]),
            "default outfit armor not a list": lambda p: p["defaultOutfit"].update(armor={}),
            "too many default outfit armor pieces": lambda p: p["defaultOutfit"].update(
                armor=[
                    {"plugin": "Skyrim.esm", "formId": i, "slotMask": 1}
                    for i in range(9)
                ]
            ),
            "modded default outfit armor": lambda p: p["defaultOutfit"]["armor"][0].update(
                plugin="Some Mod.esp"
            ),
            "zero default outfit slot mask": lambda p: p["defaultOutfit"]["armor"][0].update(slotMask=0),
            "default outfit hands not a list": lambda p: p["defaultOutfit"].update(hands=None),
            "too many default outfit hands": lambda p: p["defaultOutfit"]["hands"].append(
                {"plugin": "Skyrim.esm", "formId": 0x12EB7, "kind": "weapon", "weaponType": 1}
            ),
            "modded default outfit hand": lambda p: p["defaultOutfit"]["hands"][0].update(
                plugin="Some Mod.esp"
            ),
            "default outfit spell": lambda p: p["defaultOutfit"]["hands"][0].update(kind="spell"),
            "default outfit weapon type": lambda p: p["defaultOutfit"]["hands"][0].update(weaponType=0),
            "duplicate default outfit hand": lambda p: p["defaultOutfit"]["hands"][1].update(
                kind="weapon", weaponType=5
            ),
            "modded default outfit ammo": lambda p: p["defaultOutfit"]["ammo"].update(
                plugin="Some Mod.esp"
            ),
        }
        for name, mutate in mutations.items():
            with self.subTest(name):
                payload = valid_equipment()
                mutate(payload)
                self.assertIsNone(parse_player_equipment(payload))


class PlayerEquipmentRelayTests(unittest.IsolatedAsyncioTestCase):
    def setUp(self) -> None:
        server_app.player_appearances.clear()
        server_app.player_equipment.clear()

    def tearDown(self) -> None:
        server_app.player_appearances.clear()
        server_app.player_equipment.clear()

    async def test_relays_and_stores_equipment(self) -> None:
        emit = AsyncMock()

        with patch.object(server_app.sio, "emit", new=emit):
            result = await server_app.player_equipment_update("player-1", valid_equipment())

        self.assertEqual(result, {"ok": True})
        self.assertEqual(server_app.player_equipment["player-1"], valid_equipment())
        emit.assert_awaited_once_with(
            "player:equipment",
            {"playerId": "player-1", **valid_equipment()},
            skip_sid="player-1",
        )

    async def test_rejects_stale_revision(self) -> None:
        with patch.object(server_app.sio, "emit", new=AsyncMock()):
            await server_app.player_equipment_update("player-1", valid_equipment(2))
            result = await server_app.player_equipment_update("player-1", valid_equipment(2))

        self.assertEqual(result, {"ok": False, "error": "stale equipment revision"})

    async def test_rejects_malformed_equipment(self) -> None:
        result = await server_app.player_equipment_update("player-1", {"revision": 1})

        self.assertEqual(result, {"ok": False, "error": "invalid player equipment"})

    async def test_sends_stored_equipment_to_late_joiner(self) -> None:
        server_app.player_equipment["player-1"] = valid_equipment(3)
        server_app.player_equipment["player-2"] = valid_equipment(4)
        emit = AsyncMock()

        with patch.object(server_app.sio, "emit", new=emit):
            await server_app.connect("player-2", {})

        self.assertEqual(
            emit.await_args_list,
            [
                call("session:welcome", {"playerId": "player-2"}, to="player-2"),
                call("player:equipment", {"playerId": "player-1", **valid_equipment(3)}, to="player-2"),
            ],
        )

    async def test_forgets_equipment_on_disconnect(self) -> None:
        server_app.player_equipment["player-1"] = valid_equipment()

        with patch.object(server_app.sio, "emit", new=AsyncMock()):
            await server_app.disconnect("player-1", "client disconnect")

        self.assertNotIn("player-1", server_app.player_equipment)


class PlayerEquipmentCaptureTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addAsyncCleanup(self.temporary_directory.cleanup)
        self.original_directory = server_app.recording_store.directory
        server_app.recording_store.directory = Path(self.temporary_directory.name) / "recordings"
        server_app.player_equipment.clear()

    async def asyncTearDown(self) -> None:
        server_app.captures.stop_all("test cleanup")
        server_app.recording_store.directory = self.original_directory
        server_app.player_equipment.clear()

    async def test_captures_equipment_of_a_recorded_player_only(self) -> None:
        info = server_app.captures.start("player-1", "", "", [])

        with patch.object(server_app.sio, "emit", new=AsyncMock()):
            await server_app.player_equipment_update("player-1", valid_equipment())
            await server_app.player_equipment_update("player-2", valid_equipment())
        server_app.captures.stop(info.id)

        recording = server_app.recording_store.load(info.id)
        self.assertEqual([m.event for m in recording.messages], ["player:equipment"])
        self.assertEqual(recording.messages[0].payload, valid_equipment())
