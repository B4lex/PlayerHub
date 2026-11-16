import { describe, expect, it } from "vitest";
import type { Batch, Bot, Player, Recording } from "../api/types";
import { botPositionAt } from "./batchClock";
import { positionAt } from "./playbackClock";
import {
  batchPaused,
  batchResumed,
  batchStarted,
  playbackConfigured,
  playbackPaused,
  playbackPlayed,
  playbackRemoved,
  playbackSought,
  playerHidden,
  playerRemoved,
  playerRole,
  predicted,
  recordingStarted,
  recordingStopped,
  roleChanged,
  roleCreated,
  roleRemoved,
  type LiveState,
  type PlaybackView,
  type Prediction,
} from "./predictions";

const player: Player = {
  id: "3",
  name: "Lydia",
  address: "203.0.113.7",
  virtual: false,
  inWorld: true,
  connectedAtMs: 1000,
  interiorCell: null,
  worldspace: null,
  position: null,
  movementState: null,
  visible: 0,
  hidden: false,
  sessionId: "7",
  roleId: null,
  bytesPerSecond: 0,
  replacedPerSecond: 0,
  recordingId: null,
  playbackId: null,
};

const recording: Recording = {
  id: "rec",
  label: "Run",
  playerId: "3",
  playerName: "Lydia",
  startedAt: "2026-01-01T00:00:00Z",
  durationMs: 0,
  messageCount: 0,
  sizeBytes: 0,
  status: "recording",
  endReason: null,
};

const playback: PlaybackView = {
  id: "play",
  recordingId: "rec",
  label: "Run",
  playerId: "900",
  playerName: "Lydia",
  status: "playing",
  positionMs: 1000,
  durationMs: 10_000,
  rate: 2,
  loop: false,
  revision: 4,
  error: null,
  receivedAt: 100,
};

function bot(index: number, status: Bot["status"], positionMs: number): Bot {
  return { index, name: `Bot ${index}`, playerId: status === "pending" ? null : String(500 + index), status, startMs: 0, rate: 1, positionMs, error: null };
}

const batch: Batch = {
  id: "batch",
  recordingId: "rec",
  label: "Run",
  playerName: "Lydia",
  createdAt: "2026-01-01T00:00:00Z",
  durationMs: 10_000,
  status: "running",
  settings: { players: 2, transport: "virtual", seed: 1, jitter: 0, rateSpread: 0, rampUpSeconds: 0 },
  bots: [bot(0, "playing", 1000), bot(1, "playing", 6000)],
};

const empty: LiveState = { receivedAt: 100, players: [], recordings: [], playbacks: [], batches: [], roles: [] };

function shown(truth: LiveState, prediction: Prediction, succeeded = false): LiveState {
  return predicted(truth, [{ prediction, succeeded }]);
}

describe("predicted", () => {
  const truth = { ...empty, players: [player] };

  it("shows a command's outcome while it is in flight, whatever the server's state says", () => {
    expect(shown(truth, playerHidden(player, true)).players[0]?.hidden).toBe(true);
    expect(shown({ ...empty, players: [{ ...player, hidden: true }] }, playerHidden(player, false)).players[0]?.hidden).toBe(false);
  });

  it("keeps the outcome after the command succeeds until the server's state shows it", () => {
    const hide = playerHidden(player, true);
    expect(shown(truth, hide, true).players[0]?.hidden).toBe(true);
    expect(hide.confirmed?.(truth)).toBe(false);
    expect(hide.confirmed?.({ ...empty, players: [{ ...player, hidden: true }] })).toBe(true);
    expect(hide.confirmed?.(empty)).toBe(true);
  });

  it("drops a prediction without a confirmation as soon as its command succeeds", () => {
    const truthPlaying = { ...empty, playbacks: [playback] };
    const pause = playbackPaused(playback, 600);
    expect(shown(truthPlaying, pause).playbacks[0]?.status).toBe("paused");
    expect(shown(truthPlaying, pause, true).playbacks[0]?.status).toBe("playing");
  });

  it("applies predictions in the order they were made", () => {
    const state = predicted({ ...empty, playbacks: [playback] }, [
      { prediction: playbackPaused(playback, 600), succeeded: false },
      { prediction: playbackConfigured({ ...playback, status: "paused", positionMs: 2000, receivedAt: 600 }, 700, { loop: true }), succeeded: false },
    ]);
    expect(state.playbacks[0]).toMatchObject({ status: "paused", positionMs: 2000, loop: true });
  });
});

describe("player predictions", () => {
  it("leave the next player in a reused slot alone", () => {
    const next = { ...player, connectedAtMs: 2000 };
    const truth = { ...empty, players: [next] };
    expect(shown(truth, playerRemoved(player)).players).toEqual([next]);
    expect(playerRemoved(player).confirmed?.(truth)).toBe(true);
    expect(shown(truth, playerHidden(player, true)).players[0]?.hidden).toBe(false);
  });
});

describe("recording predictions", () => {
  it("list a capture from its response and mark its player", () => {
    const start = recordingStarted(recording);
    const state = shown({ ...empty, players: [player] }, start, true);
    expect(state.recordings).toEqual([recording]);
    expect(state.players[0]?.recordingId).toBe("rec");
    expect(start.confirmed?.({ ...empty, recordings: [recording] })).toBe(true);
  });

  it("end a capture and free its player", () => {
    const truth = { ...empty, players: [{ ...player, recordingId: "rec" }], recordings: [recording] };
    const stop = recordingStopped("rec");
    const state = shown(truth, stop);
    expect(state.recordings[0]).toMatchObject({ status: "complete", endReason: "stopped" });
    expect(state.players[0]?.recordingId).toBeNull();
    expect(stop.confirmed?.(truth)).toBe(false);
    expect(stop.confirmed?.({ ...empty, recordings: [{ ...recording, status: "complete" }] })).toBe(true);
  });
});

describe("playback predictions", () => {
  const truth = { ...empty, playbacks: [playback] };

  it("pause where the replay has got to", () => {
    expect(shown(truth, playbackPaused(playback, 600)).playbacks[0]).toMatchObject({ status: "paused", positionMs: 2000 });
  });

  it("play on from the position, or from the start once finished", () => {
    const paused = { ...playback, status: "paused" as const, positionMs: 3000 };
    const resumed = shown({ ...empty, playbacks: [paused] }, playbackPlayed(paused, 900)).playbacks[0]!;
    expect(positionAt(resumed, 1000 - resumed.receivedAt)).toBe(3200);

    const ended = { ...playback, status: "ended" as const, positionMs: 10_000 };
    expect(shown({ ...empty, playbacks: [ended] }, playbackPlayed(ended, 900)).playbacks[0]).toMatchObject({ status: "playing", positionMs: 0 });
  });

  it("seek within the recording, leaving a finished replay paused", () => {
    expect(shown(truth, playbackSought(playback, 600, 50_000)).playbacks[0]).toMatchObject({ status: "playing", positionMs: 10_000, receivedAt: 600 });
    const ended = { ...playback, status: "ended" as const, positionMs: 10_000 };
    expect(shown({ ...empty, playbacks: [ended] }, playbackSought(ended, 600, 4000)).playbacks[0]).toMatchObject({ status: "paused", positionMs: 4000 });
  });

  it("change the rate from the current position", () => {
    const changed = shown(truth, playbackConfigured(playback, 600, { rate: 0.5 })).playbacks[0]!;
    expect(changed).toMatchObject({ rate: 0.5, positionMs: 2000 });
    expect(positionAt(changed, 800 - changed.receivedAt)).toBe(2100);
  });

  it("remove a closed replay with its virtual player", () => {
    const virtual = { ...player, id: "900", virtual: true, playbackId: "play" };
    const state = shown({ ...truth, players: [player, virtual] }, playbackRemoved("play"));
    expect(state.playbacks).toEqual([]);
    expect(state.players).toEqual([player]);
  });
});

describe("batch predictions", () => {
  const truth = { ...empty, batches: [batch] };
  const positions = (state: LiveState, now: number) =>
    state.batches[0]!.bots.map((each) => botPositionAt(each, batch.durationMs, now - state.receivedAt));

  it("pause every bot where it has got to", () => {
    const pause = batchPaused("batch", 2100);
    const state = shown(truth, pause);
    expect(state.batches[0]?.status).toBe("paused");
    expect(positions(state, 9000)).toEqual([3000, 8000]);
    expect(pause.confirmed?.(truth)).toBe(false);
    expect(pause.confirmed?.(empty)).toBe(true);
  });

  it("resume from the paused positions, however old the snapshot", () => {
    const paused: Batch = { ...batch, status: "paused", bots: [bot(0, "paused", 1000), bot(1, "pending", 0)] };
    const state = shown({ ...empty, batches: [paused] }, batchResumed("batch", 60_100));
    expect(state.batches[0]?.status).toBe("starting");
    expect(positions(state, 60_100)).toEqual([1000, 0]);
    expect(positions(state, 60_600)).toEqual([1500, 0]);
  });

  it("list a batch from its response, with its bots where the server put them", () => {
    const start = batchStarted(batch, 5100);
    const state = shown(empty, start, true);
    expect(positions(state, 5100)).toEqual([1000, 6000]);
    expect(shown(truth, start, true).batches).toEqual([batch]);
  });
});

describe("role predictions", () => {
  const role = { id: "2", name: "Moderator", color: "#3366ff", holders: 1 };
  const truth = { ...empty, players: [{ ...player, roleId: "2" }], roles: [role] };

  it("show a player's role until the server's state does", () => {
    const give = playerRole(player, "2");
    expect(shown({ ...empty, players: [player] }, give, true).players[0]?.roleId).toBe("2");
    expect(give.confirmed?.(truth)).toBe(true);
    expect(playerRole(player, null).confirmed?.(truth)).toBe(false);
  });

  it("list a created role once", () => {
    const created = roleCreated(role);
    expect(shown(empty, created, true).roles).toEqual([role]);
    expect(shown(truth, created).roles).toEqual([role]);
    expect(created.confirmed?.(truth)).toBe(true);
  });

  it("show a role's new name and color, whatever the color's case", () => {
    const changed = roleChanged("2", "Staff", "#FF0000");
    expect(shown(truth, changed).roles[0]).toMatchObject({ name: "Staff", color: "#FF0000", holders: 1 });
    expect(changed.confirmed?.(truth)).toBe(false);
    expect(changed.confirmed?.({ ...truth, roles: [{ ...role, name: "Staff", color: "#ff0000" }] })).toBe(true);
    expect(changed.confirmed?.(empty)).toBe(true);
  });

  it("take a deleted role from its holders", () => {
    const state = shown(truth, roleRemoved("2"));
    expect(state.roles).toEqual([]);
    expect(state.players[0]?.roleId).toBeNull();
  });
});
