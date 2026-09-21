import { describe, expect, it } from "vitest";
import { formatBytes, formatFormId, formatRelative, formatTimestamp, plural } from "./format";
import { newerPlayback, positionAt } from "./playbackClock";

const timeline = { status: "playing", positionMs: 1000, durationMs: 10_000, rate: 1, loop: false } as const;

describe("positionAt", () => {
  it("advances at the playback rate while playing", () => {
    expect(positionAt(timeline, 500)).toBe(1500);
    expect(positionAt({ ...timeline, rate: 2 }, 500)).toBe(2000);
  });

  it("stands still unless playing", () => {
    expect(positionAt({ ...timeline, status: "paused" }, 5000)).toBe(1000);
    expect(positionAt({ ...timeline, status: "ended", positionMs: 10_000 }, 5000)).toBe(10_000);
  });

  it("stops at the end, or wraps when looping", () => {
    expect(positionAt(timeline, 20_000)).toBe(10_000);
    expect(positionAt({ ...timeline, loop: true }, 10_500)).toBe(1500);
  });

  it("ignores a clock that ran backwards", () => {
    expect(positionAt(timeline, -300)).toBe(1000);
  });
});

describe("newerPlayback", () => {
  it("keeps the held state when it is newer than the one that arrived", () => {
    const held = { revision: 5, tag: "held" };
    expect(newerPlayback(held, { revision: 4, tag: "arrived" })).toBe(held);
  });

  it("takes the arrived state when it is equal or newer, or nothing is held", () => {
    const arrived = { revision: 5, tag: "arrived" };
    expect(newerPlayback({ revision: 5, tag: "held" }, arrived)).toBe(arrived);
    expect(newerPlayback({ revision: 4, tag: "held" }, arrived)).toBe(arrived);
    expect(newerPlayback(undefined, arrived)).toBe(arrived);
  });
});

describe("formatting", () => {
  it("formats timestamps", () => {
    expect(formatTimestamp(0)).toBe("0:00");
    expect(formatTimestamp(65_300)).toBe("1:05");
    expect(formatTimestamp(65_300, true)).toBe("1:05.3");
    expect(formatTimestamp(3_725_000)).toBe("1:02:05");
    expect(formatTimestamp(-5)).toBe("0:00");
    expect(formatTimestamp(59_999, true)).toBe("0:59.9");
  });

  it("formats sizes", () => {
    expect(formatBytes(512)).toBe("512 B");
    expect(formatBytes(617_986)).toBe("604 KB");
    expect(formatBytes(5 * 1024 * 1024)).toBe("5.0 MB");
  });

  it("formats relative times", () => {
    const now = Date.parse("2026-09-21T12:00:00Z");
    expect(formatRelative("2026-09-21T11:59:50Z", now)).toBe("just now");
    expect(formatRelative("2026-09-21T11:55:00Z", now)).toBe("5 min ago");
    expect(formatRelative("2026-09-21T09:00:00Z", now)).toBe("3 h ago");
    expect(formatRelative("garbage", now)).toBe("garbage");
  });

  it("formats form ids and plurals", () => {
    expect(formatFormId(0x3c)).toBe("0x0000003C");
    expect(plural(1, "player")).toBe("1 player");
    expect(plural(2, "player")).toBe("2 players");
  });
});
