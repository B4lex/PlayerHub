import type { Playback } from "../api/types";

type Timeline = Pick<Playback, "status" | "positionMs" | "durationMs" | "rate" | "loop">;

/**
 * Where a playback is now, given the state the server last reported and how long
 * ago that was. The server pushes state only when something changes, so while a
 * replay is playing the panel advances the position itself.
 */
export function positionAt(playback: Timeline, elapsedMs: number): number {
  if (playback.status !== "playing") return playback.positionMs;
  const position = playback.positionMs + Math.max(0, elapsedMs) * playback.rate;
  if (position < playback.durationMs) return position;
  return playback.loop && playback.durationMs > 0 ? position % playback.durationMs : playback.durationMs;
}

/**
 * Choose between the state already held and one that just arrived, whichever is
 * newer. A command's response and a state push can cross on the wire, so the
 * arrival order says nothing about which is current.
 */
export function newerPlayback<T extends Pick<Playback, "revision">>(held: T | undefined, arrived: T): T {
  return held !== undefined && held.revision > arrived.revision ? held : arrived;
}
