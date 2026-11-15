import type { Batch, Bot, Player, Playback, PlaybackSettings, Recording } from "../api/types";
import { botPositionAt } from "./batchClock";
import { positionAt } from "./playbackClock";

/** A playback plus the local time its state arrived, so its position can be extrapolated. */
export interface PlaybackView extends Playback {
  receivedAt: number;
}

/** The part of the server's state that commands change. */
export interface LiveState {
  /** Local `performance.now()` when the snapshot arrived; bot positions are extrapolated from it. */
  receivedAt: number;
  players: Player[];
  recordings: Recording[];
  playbacks: PlaybackView[];
  batches: Batch[];
}

/**
 * What a command does to the state, shown from the moment it is sent. It is
 * dropped when the command fails, and otherwise once the server's own state
 * shows the outcome.
 */
export interface Prediction {
  /** A new prediction replaces the earlier ones with its key. */
  key?: string;
  apply: (state: LiveState) => LiveState;
  /**
   * Whether the server's state shows the outcome, or can no longer show it.
   * Without it the prediction lasts only while the command is in flight, for
   * commands whose response carries the new state.
   */
  confirmed?: (state: LiveState) => boolean;
}

function sameConnection(player: Player): (other: Player) => boolean {
  // Slots are reused, so the ID alone could name the next player to connect.
  return (other) => other.id === player.id && other.connectedAtMs === player.connectedAtMs;
}

function replace<T extends { id: string }>(items: T[], id: string, change: (item: T) => T): T[] {
  return items.map((item) => (item.id === id ? change(item) : item));
}

function without<T extends { id: string }>(items: T[], id: string): T[] {
  return items.filter((item) => item.id !== id);
}

function has(items: { id: string }[], id: string): boolean {
  return items.some((item) => item.id === id);
}

export function playerHidden(player: Player, hidden: boolean): Prediction {
  const same = sameConnection(player);
  return {
    key: `player:${player.id}:hidden`,
    apply: (state) => ({ ...state, players: state.players.map((other) => (same(other) ? { ...other, hidden } : other)) }),
    confirmed: (state) => (state.players.find(same)?.hidden ?? hidden) === hidden,
  };
}

/** A kick or a ban. */
export function playerRemoved(player: Player): Prediction {
  const same = sameConnection(player);
  return {
    key: `player:${player.id}:listed`,
    apply: (state) => ({ ...state, players: state.players.filter((other) => !same(other)) }),
    confirmed: (state) => !state.players.some(same),
  };
}

/** A capture the server has answered with, listed before a snapshot does. */
export function recordingStarted(recording: Recording): Prediction {
  return {
    key: `recording:${recording.id}:listed`,
    apply: (state) =>
      has(state.recordings, recording.id)
        ? state
        : {
            ...state,
            recordings: [recording, ...state.recordings],
            players: state.players.map((player) =>
              player.id === recording.playerId && !player.virtual && player.recordingId === null
                ? { ...player, recordingId: recording.id }
                : player,
            ),
          },
    confirmed: (state) => has(state.recordings, recording.id),
  };
}

export function recordingStopped(id: string): Prediction {
  return {
    key: `recording:${id}:status`,
    apply: (state) => ({
      ...state,
      recordings: replace(state.recordings, id, (recording) =>
        recording.status === "recording" ? { ...recording, status: "complete", endReason: "stopped" } : recording,
      ),
      players: state.players.map((player) => (player.recordingId === id ? { ...player, recordingId: null } : player)),
    }),
    confirmed: (state) => state.recordings.find((recording) => recording.id === id)?.status !== "recording",
  };
}

export function recordingRemoved(id: string): Prediction {
  return {
    key: `recording:${id}:listed`,
    apply: (state) => ({ ...state, recordings: without(state.recordings, id) }),
    confirmed: (state) => !has(state.recordings, id),
  };
}

function playbackChanged(id: string, change: (playback: PlaybackView) => Partial<PlaybackView>): Prediction {
  return {
    apply: (state) => ({
      ...state,
      playbacks: replace(state.playbacks, id, (playback) => ({ ...playback, ...change(playback) })),
    }),
  };
}

// The playback predictions take the deck's playback as shown and the local time of the click.

export function playbackPaused(shown: PlaybackView, at: number): Prediction {
  const positionMs = positionAt(shown, at - shown.receivedAt);
  return playbackChanged(shown.id, () => ({ status: "paused", positionMs, receivedAt: at }));
}

export function playbackPlayed(shown: PlaybackView, at: number): Prediction {
  const reached = positionAt(shown, at - shown.receivedAt);
  const positionMs = shown.status === "ended" || reached >= shown.durationMs ? 0 : reached;
  return playbackChanged(shown.id, () => ({ status: "playing", positionMs, receivedAt: at }));
}

export function playbackSought(shown: PlaybackView, at: number, positionMs: number): Prediction {
  return playbackChanged(shown.id, (playback) => ({
    status: playback.status === "ended" ? "paused" : playback.status,
    positionMs: Math.min(Math.max(positionMs, 0), playback.durationMs),
    receivedAt: at,
  }));
}

export function playbackConfigured(shown: PlaybackView, at: number, settings: PlaybackSettings): Prediction {
  // A new rate counts from where the replay is now.
  const positionMs = positionAt(shown, at - shown.receivedAt);
  return playbackChanged(shown.id, () => ({ ...settings, positionMs, receivedAt: at }));
}

/** A closed replay, with its virtual player. */
export function playbackRemoved(id: string): Prediction {
  return {
    key: `playback:${id}:listed`,
    apply: (state) => ({
      ...state,
      playbacks: without(state.playbacks, id),
      players: state.players.filter((player) => player.playbackId !== id),
    }),
    confirmed: (state) => !has(state.playbacks, id),
  };
}

/**
 * A bot that started playing at `at`, placed so that extrapolating from the
 * snapshot's arrival, as the batch view does, finds it at `bot.positionMs` then.
 */
function playingSince(bot: Bot, at: number, state: LiveState): Bot {
  return { ...bot, status: "playing", positionMs: bot.positionMs - bot.rate * Math.max(0, at - state.receivedAt) };
}

/** A batch the server has answered with at `at`, listed before a snapshot does. */
export function batchStarted(batch: Batch, at: number): Prediction {
  return {
    key: `batch:${batch.id}:listed`,
    apply: (state) =>
      has(state.batches, batch.id)
        ? state
        : {
            ...state,
            batches: [
              ...state.batches,
              { ...batch, bots: batch.bots.map((bot) => (bot.status === "playing" ? playingSince(bot, at, state) : bot)) },
            ],
          },
    confirmed: (state) => has(state.batches, batch.id),
  };
}

export function batchPaused(id: string, at: number): Prediction {
  return {
    key: `batch:${id}:status`,
    apply: (state) => ({
      ...state,
      batches: replace(state.batches, id, (batch) =>
        batch.status === "paused"
          ? batch
          : {
              ...batch,
              status: "paused",
              bots: batch.bots.map((bot) =>
                bot.status === "playing"
                  ? { ...bot, status: "paused", positionMs: botPositionAt(bot, batch.durationMs, at - state.receivedAt) }
                  : bot,
              ),
            },
      ),
    }),
    confirmed: (state) => (state.batches.find((batch) => batch.id === id)?.status ?? "paused") === "paused",
  };
}

export function batchResumed(id: string, at: number): Prediction {
  return {
    key: `batch:${id}:status`,
    apply: (state) => ({
      ...state,
      batches: replace(state.batches, id, (batch) =>
        batch.status !== "paused"
          ? batch
          : {
              ...batch,
              status: batch.bots.some((bot) => bot.status === "pending") ? "starting" : "running",
              bots: batch.bots.map((bot) => (bot.status === "paused" ? playingSince(bot, at, state) : bot)),
            },
      ),
    }),
    confirmed: (state) => state.batches.find((batch) => batch.id === id)?.status !== "paused",
  };
}

export function batchRemoved(id: string): Prediction {
  return {
    key: `batch:${id}:listed`,
    apply: (state) => ({ ...state, batches: without(state.batches, id) }),
    confirmed: (state) => !has(state.batches, id),
  };
}

export interface HeldPrediction {
  prediction: Prediction;
  /** The command has succeeded, so the prediction only waits for the server's state to show it. */
  succeeded: boolean;
}

export function awaited(held: HeldPrediction, truth: LiveState): boolean {
  return !held.succeeded || held.prediction.confirmed?.(truth) === false;
}

/** The server's state with the predictions still awaited applied, oldest first. */
export function predicted<T extends HeldPrediction>(truth: LiveState, held: readonly T[]): LiveState {
  return held.reduce((state, entry) => (awaited(entry, truth) ? entry.prediction.apply(state) : state), truth);
}
