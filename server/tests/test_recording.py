import tempfile
import unittest
from datetime import UTC, datetime
from pathlib import Path

from recording_fixtures import (
    action,
    appearance,
    checkpoint,
    equipment,
    header,
    message,
    recording,
    timeline,
    write_recording,
)

from server.recording import (
    AlreadyCapturing,
    CaptureManager,
    RecordingError,
    RecordingNotFound,
    RecordingStore,
    summarize_markers,
)
from server.recording.format import read_recording, scan_recording_info


class TemporaryDirectoryTestCase(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary_directory.cleanup)
        self.directory = Path(self.temporary_directory.name) / "recordings"


class ReadRecordingTests(TemporaryDirectoryTestCase):
    def test_round_trips_a_recording(self) -> None:
        original = timeline()
        path = write_recording(self.directory / "a.jsonl", original)

        loaded = read_recording(path)

        self.assertEqual(loaded.header, original.header)
        self.assertEqual(loaded.messages, original.messages)
        self.assertEqual(loaded.end, original.end)
        self.assertEqual(loaded.duration_ms, 1000)

    def test_duration_of_an_unfinished_recording_is_its_last_message(self) -> None:
        path = write_recording(
            self.directory / "a.jsonl",
            recording([message(0, "player:update", checkpoint(1)), message(40, "player:update", checkpoint(2))]),
        )

        loaded = read_recording(path)

        self.assertIsNone(loaded.end)
        self.assertEqual(loaded.duration_ms, 40)

    def test_ignores_a_torn_final_line(self) -> None:
        path = write_recording(
            self.directory / "a.jsonl", recording([message(0, "player:update", checkpoint(1))])
        )
        with path.open("a", encoding="utf-8") as file:
            file.write('{"type":"message","t":50,"event":"player:upd')

        loaded = read_recording(path)

        self.assertEqual(len(loaded.messages), 1)

    def test_rejects_invalid_content(self) -> None:
        cases = {
            "corrupt middle line": ["not json", '{"type":"end","t":1,"reason":"x"}'],
            "unknown event": ['{"type":"message","t":0,"event":"player:dance","payload":{}}'],
            "invalid payload": ['{"type":"message","t":0,"event":"player:update","payload":{"x":1}}'],
            "presence with payload": ['{"type":"message","t":0,"event":"player:exit","payload":{"x":1}}'],
            "unknown line type": ['{"type":"marker","t":0}'],
            "missing timestamp": ['{"type":"end","reason":"x"}'],
        }
        for name, lines in cases.items():
            with self.subTest(name):
                path = write_recording(self.directory / "a.jsonl", recording([]))
                with path.open("a", encoding="utf-8") as file:
                    file.write("\n".join([*lines, '{"type":"end","t":2,"reason":"x"}']) + "\n")
                with self.assertRaises(RecordingError):
                    read_recording(path)

    def test_reads_world_presence(self) -> None:
        path = write_recording(
            self.directory / "a.jsonl",
            recording([message(0, "player:exit", {}), message(10, "player:enter", {})], 10),
        )

        loaded = read_recording(path)

        self.assertEqual([(m.event, m.payload) for m in loaded.messages], [("player:exit", {}), ("player:enter", {})])

    def test_rejects_timestamps_going_backwards(self) -> None:
        path = write_recording(
            self.directory / "a.jsonl",
            recording(
                [message(50, "player:update", checkpoint(1)), message(10, "player:update", checkpoint(2))]
            ),
        )

        with self.assertRaisesRegex(RecordingError, "backwards"):
            read_recording(path)

    def test_rejects_other_formats(self) -> None:
        path = self.directory / "a.jsonl"
        path.parent.mkdir()
        path.write_text('{"type":"header","format":"other","version":2}\n', encoding="utf-8")

        with self.assertRaisesRegex(RecordingError, "not a"):
            read_recording(path)


class ScanRecordingInfoTests(TemporaryDirectoryTestCase):
    def test_summarizes_a_finished_recording_without_reading_payloads(self) -> None:
        path = write_recording(self.directory / "20260921-101500-abc123.jsonl", timeline())

        info = scan_recording_info(path)

        self.assertEqual(info.status, "complete")
        self.assertEqual(info.end_reason, "stopped")
        self.assertEqual(info.duration_ms, 1000)
        self.assertEqual(info.message_count, len(timeline().messages))
        self.assertEqual(info.player_name, "Lydia Stormblade")

    def test_marks_a_recording_without_an_end_as_interrupted(self) -> None:
        path = write_recording(
            self.directory / "a.jsonl", recording([message(25, "player:update", checkpoint(1))])
        )
        with path.open("a", encoding="utf-8") as file:
            file.write('{"type":"message","t":90,"ev')  # torn

        info = scan_recording_info(path)

        self.assertEqual(info.status, "interrupted")
        self.assertEqual(info.duration_ms, 25)
        self.assertEqual(info.message_count, 1)


class RecordingStoreTests(TemporaryDirectoryTestCase):
    def test_lists_newest_first_and_caches_until_a_file_changes(self) -> None:
        store = RecordingStore(self.directory)
        for stamp, started in (("20260101-000000-aaaaaa", "2026-01-01T00:00:00Z"), ("20260201-000000-bbbbbb", "2026-02-01T00:00:00Z")):
            value = timeline()
            value.header = header(stamp, started_at=started)
            write_recording(self.directory / f"{stamp}.jsonl", value)

        infos = store.infos()

        self.assertEqual([info.id for info in infos], ["20260201-000000-bbbbbb", "20260101-000000-aaaaaa"])
        self.assertIs(store.infos()[0], infos[0])

    def test_skips_unreadable_files_and_mismatched_ids(self) -> None:
        store = RecordingStore(self.directory)
        self.directory.mkdir()
        (self.directory / "broken.jsonl").write_text("nonsense\n", encoding="utf-8")
        write_recording(self.directory / "renamed.jsonl", timeline())  # header id differs

        self.assertEqual(store.infos(), [])

    def test_rejects_ids_that_could_escape_the_directory(self) -> None:
        store = RecordingStore(self.directory)

        for bad in ("../secret", "a/b", "", ".hidden", "x" * 65, "a b"):
            with self.subTest(bad):
                with self.assertRaises(RecordingNotFound):
                    store.path_for(bad)

    def test_loads_and_deletes(self) -> None:
        store = RecordingStore(self.directory)
        write_recording(self.directory / "20260921-101500-abc123.jsonl", timeline())

        self.assertEqual(store.load("20260921-101500-abc123").duration_ms, 1000)
        store.delete("20260921-101500-abc123")

        with self.assertRaises(RecordingNotFound):
            store.load("20260921-101500-abc123")
        with self.assertRaises(RecordingNotFound):
            store.delete("20260921-101500-abc123")
        self.assertEqual(store.infos(), [])


class CaptureManagerTests(TemporaryDirectoryTestCase):
    def make_manager(self):
        self.now_ns = 1_000_000_000
        self.changes = 0
        store = RecordingStore(self.directory)

        def on_change() -> None:
            self.changes += 1

        manager = CaptureManager(
            store,
            on_change=on_change,
            monotonic_ns=lambda: self.now_ns,
            now=lambda: datetime(2026, 9, 21, 10, 15, tzinfo=UTC),
        )
        self.addCleanup(manager.stop_all, "test cleanup")
        return store, manager

    def test_records_the_initial_state_then_accepted_messages_until_stopped(self) -> None:
        store, manager = self.make_manager()

        info = manager.start(
            "player-1",
            "Lydia",
            "  Boss fight  ",
            [("player:appearance", appearance()), ("player:update", checkpoint(7))],
        )
        self.now_ns += 75_000_000
        manager.record("player-1", "player:update", checkpoint(8))
        manager.record("someone-else", "player:update", checkpoint(1))  # not captured
        self.now_ns += 25_000_000
        stopped = manager.stop(info.id)

        self.assertEqual(info.status, "recording")
        self.assertEqual(info.label, "Boss fight")
        self.assertEqual(stopped.status, "complete")
        self.assertEqual(stopped.duration_ms, 100)
        loaded = store.load(info.id)
        self.assertEqual(
            [(m.t_ms, m.event) for m in loaded.messages],
            [(0, "player:appearance"), (0, "player:update"), (75, "player:update")],
        )
        self.assertEqual(loaded.header.player_name, "Lydia")
        self.assertFalse(manager.is_capturing("player-1"))
        self.assertGreater(self.changes, 0)

    def test_derives_a_label_when_none_is_given(self) -> None:
        _, manager = self.make_manager()

        info = manager.start("player-1", "Lydia", "", [])

        self.assertTrue(info.label.startswith("Lydia 2026-09-21"))

    def test_refuses_a_second_capture_of_the_same_player(self) -> None:
        _, manager = self.make_manager()
        first = manager.start("player-1", "", "", [])

        with self.assertRaises(AlreadyCapturing) as caught:
            manager.start("player-1", "", "", [])

        self.assertEqual(caught.exception.recording_id, first.id)
        self.assertEqual(manager.recording_id_for("player-1"), first.id)

    def test_reports_live_progress_while_capturing(self) -> None:
        _, manager = self.make_manager()
        info = manager.start("player-1", "", "", [("player:update", checkpoint(1))])
        self.now_ns += 2_000_000_000
        manager.record("player-1", "player:update", checkpoint(2))

        [live] = manager.active_infos()

        self.assertEqual(live.id, info.id)
        self.assertEqual((live.status, live.duration_ms, live.message_count), ("recording", 2000, 2))
        self.assertGreater(live.size_bytes, 0)
        self.assertTrue(manager.is_active(info.id))

    def test_ends_the_capture_when_the_player_disconnects(self) -> None:
        store, manager = self.make_manager()
        info = manager.start("player-1", "", "", [])

        manager.stop_player("player-1", "disconnected")

        self.assertEqual(store.info(info.id).end_reason, "disconnected")
        self.assertEqual(manager.active_infos(), [])

    def test_stop_all_finishes_every_capture(self) -> None:
        store, manager = self.make_manager()
        ids = [manager.start(player, "", "", []).id for player in ("a", "b")]

        manager.stop_all("shutdown")

        self.assertEqual([store.info(recording_id).end_reason for recording_id in ids], ["shutdown"] * 2)

    def test_stopping_an_unknown_capture_is_an_error(self) -> None:
        _, manager = self.make_manager()

        with self.assertRaises(RecordingNotFound):
            manager.stop("20260921-101500-abc123")

    def test_a_disk_failure_ends_the_capture_without_raising_into_sync(self) -> None:
        store, manager = self.make_manager()
        info = manager.start("player-1", "", "", [])
        capture = manager._by_player["player-1"]
        capture.writer._file.close()  # release the real file, then swap in one that fails

        class FailingFile:
            def write(self, _: str) -> None:
                raise OSError("disk full")

            def flush(self) -> None:
                pass

            def close(self) -> None:
                pass

        capture.writer._file = FailingFile()

        manager.record("player-1", "player:update", checkpoint(1))

        self.assertFalse(manager.is_capturing("player-1"))
        self.assertEqual(store.info(info.id).status, "interrupted")


class SummarizeMarkersTests(unittest.TestCase):
    def test_marks_attacks_gear_changes_stance_furniture_and_areas(self) -> None:
        messages = [
            message(0, "player:appearance", appearance(1)),
            message(0, "player:equipment", equipment(1)),
            message(0, "player:update", checkpoint(1)),
            message(100, "player:update", checkpoint(2, weaponDrawn=True)),
            message(150, "animation:action", action(1, 2, power=True, direction="forward")),
            message(200, "animation:action", action(2, 2, action="bash")),
            message(300, "player:equipment", equipment(2, armor_form_id=0x999)),
            message(400, "player:appearance", appearance(2, name="Renamed")),
            message(500, "player:update", checkpoint(3, weaponDrawn=True, furnitureRefId=0xABC)),
            message(600, "player:update", checkpoint(4, weaponDrawn=False, cellId=9, worldspaceId=0)),
            message(700, "player:update", checkpoint(5, cellId=9, worldspaceId=0)),  # same interior
            message(800, "player:update", checkpoint(6, cellId=2, worldspaceId=0)),
        ]

        markers = summarize_markers(recording(messages, 800))

        self.assertEqual(
            [(marker.t_ms, marker.kind, marker.label) for marker in markers],
            [
                (100, "weapon", "Weapon drawn"),
                (150, "attack", "Power attack (forward)"),
                (200, "attack", "Bash"),
                (300, "equipment", "Equipment changed"),
                (400, "appearance", "Appearance changed"),
                (500, "furniture", "Started using furniture"),
                (600, "weapon", "Weapon sheathed"),
                (600, "furniture", "Left furniture"),
                (600, "area", "Entered interior 0x00000009"),
                (800, "area", "Entered interior 0x00000002"),
            ],
        )


if __name__ == "__main__":
    unittest.main()
