import { useEffect, useState } from "react";
import { positionAt } from "../../lib/playbackClock";
import type { PlaybackView } from "../../state/ServerState";

/** How often the displayed position refreshes while playing. */
const REFRESH_MS = 50;

/**
 * The live position of a playback in milliseconds. The server reports a
 * position only when something changes; while playing, this carries it forward
 * with the local clock.
 */
export function usePlaybackPosition(playback: PlaybackView): number {
  const [now, setNow] = useState(() => performance.now());
  const playing = playback.status === "playing";

  useEffect(() => {
    if (!playing) return;
    let frame = 0;
    let last = 0;
    const tick = (time: number) => {
      if (time - last >= REFRESH_MS) {
        last = time;
        setNow(time);
      }
      frame = requestAnimationFrame(tick);
    };
    frame = requestAnimationFrame(tick);
    return () => cancelAnimationFrame(frame);
  }, [playing]);

  return positionAt(playback, now - playback.receivedAt);
}
