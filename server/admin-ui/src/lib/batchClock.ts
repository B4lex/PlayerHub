import type { Bot } from "../api/types";
import { positionAt } from "./playbackClock";

/**
 * Where a bot is now in its looping replay, given the state the server last
 * reported and how long ago that was. Like a playback's, a bot's position is
 * not pushed while it plays.
 */
export function botPositionAt(bot: Pick<Bot, "status" | "positionMs" | "rate">, durationMs: number, elapsedMs: number): number {
  const status = bot.status === "playing" ? "playing" : "paused";
  return positionAt({ status, positionMs: bot.positionMs, durationMs, rate: bot.rate, loop: true }, elapsedMs);
}
