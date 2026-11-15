import { useEffect, useState } from "react";

/** Re-renders the component every `intervalMs` and returns the current `performance.now()`. */
export function useTicker(intervalMs: number, enabled = true): number {
  const [now, setNow] = useState(() => performance.now());
  useEffect(() => {
    if (!enabled) return;
    const timer = window.setInterval(() => setNow(performance.now()), intervalMs);
    return () => window.clearInterval(timer);
  }, [intervalMs, enabled]);
  // Read afresh while ticking: the render that enables the ticker, or one between two ticks, would
  // otherwise place what it extrapolates at the time of the last tick.
  return enabled ? performance.now() : now;
}
