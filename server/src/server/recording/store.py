"""Recording files on disk: create, list, load, and delete."""

import logging
import re
import secrets
from collections.abc import Collection
from datetime import datetime
from pathlib import Path
from typing import TextIO

from server.recording.format import (
    Recording,
    RecordedMessage,
    RecordingEnd,
    RecordingError,
    RecordingHeader,
    RecordingInfo,
    encode_end,
    encode_header,
    encode_message,
    read_recording,
    scan_recording_info,
)

logger = logging.getLogger("uvicorn.error")

SUFFIX = ".jsonl"
# Recording IDs become file names and appear in URLs, so keep them to a safe alphabet.
_ID_PATTERN = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,63}")


class RecordingNotFound(LookupError):
    pass


def is_valid_recording_id(recording_id: str) -> bool:
    return _ID_PATTERN.fullmatch(recording_id) is not None


class RecordingWriter:
    """Appends lines to one recording file; every line is flushed as it is written."""

    def __init__(self, path: Path, header: RecordingHeader) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        # Exclusive creation: a capture never overwrites an existing recording.
        self._file: TextIO | None = path.open("x", encoding="utf-8", newline="\n")
        self.path = path
        self.bytes_written = 0
        self._write_line(encode_header(header))

    def _write_line(self, line: str) -> None:
        if self._file is None:
            raise ValueError("recording writer is closed")
        data = line + "\n"
        self._file.write(data)
        self._file.flush()
        self.bytes_written += len(data.encode("utf-8"))

    def write(self, message: RecordedMessage) -> None:
        self._write_line(encode_message(message))

    def close(self, end: RecordingEnd | None) -> None:
        """Finish the file. Pass ``None`` when the disk failed, so no end line is attempted."""
        if self._file is None:
            return
        try:
            if end is not None:
                self._write_line(encode_end(end))
        finally:
            self._file.close()
            self._file = None


class RecordingStore:
    def __init__(self, directory: Path) -> None:
        self.directory = directory
        self._info_cache: dict[Path, tuple[int, int, RecordingInfo]] = {}

    def new_id(self, now: datetime) -> str:
        """A sortable, human-readable ID that is unique within this directory."""
        stamp = now.strftime("%Y%m%d-%H%M%S")
        while True:
            recording_id = f"{stamp}-{secrets.token_hex(3)}"
            if not self.path_for(recording_id).exists():
                return recording_id

    def path_for(self, recording_id: str) -> Path:
        if not is_valid_recording_id(recording_id):
            raise RecordingNotFound(recording_id)
        return self.directory / f"{recording_id}{SUFFIX}"

    def create(self, header: RecordingHeader) -> RecordingWriter:
        return RecordingWriter(self.path_for(header.id), header)

    def infos(self, skip: Collection[str] = ()) -> list[RecordingInfo]:
        """Metadata of every readable recording, newest first.

        ``skip`` names recordings the caller already describes better, such as
        captures still being written, which would otherwise be rescanned as they grow.
        """
        if not self.directory.is_dir():
            return []
        infos: list[RecordingInfo] = []
        seen: set[Path] = set()
        for path in self.directory.glob(f"*{SUFFIX}"):
            seen.add(path)
            if path.stem in skip:
                continue
            info = self._info(path)
            if info is not None:
                infos.append(info)
        for stale in set(self._info_cache) - seen:
            del self._info_cache[stale]
        infos.sort(key=lambda info: (info.started_at, info.id), reverse=True)
        return infos

    def _info(self, path: Path) -> RecordingInfo | None:
        try:
            stat = path.stat()
        except OSError:
            return None
        cached = self._info_cache.get(path)
        if cached is not None and cached[:2] == (stat.st_mtime_ns, stat.st_size):
            return cached[2]
        try:
            info = scan_recording_info(path)
        except RecordingError as error:
            logger.warning("Skipping unreadable recording %s: %s", path.name, error)
            return None
        if info.id != path.stem:
            logger.warning("Skipping recording %s: header id %s does not match", path.name, info.id)
            return None
        self._info_cache[path] = (stat.st_mtime_ns, stat.st_size, info)
        return info

    def info(self, recording_id: str) -> RecordingInfo:
        info = self._info(self.path_for(recording_id))
        if info is None:
            raise RecordingNotFound(recording_id)
        return info

    def load(self, recording_id: str) -> Recording:
        path = self.path_for(recording_id)
        if not path.is_file():
            raise RecordingNotFound(recording_id)
        return read_recording(path)

    def delete(self, recording_id: str) -> None:
        path = self.path_for(recording_id)
        try:
            path.unlink()
        except FileNotFoundError as error:
            raise RecordingNotFound(recording_id) from error
        self._info_cache.pop(path, None)
