import { useEffect, useState } from "react";

/** Re-renders the component every `intervalMs` and returns the current `performance.now()`. */
export function useTicker(intervalMs: number, enabled = true): number {
  const [now, setNow] = useState(() => performance.now());
  useEffect(() => {
    if (!enabled) return;
    const timer = window.setInterval(() => setNow(performance.now()), intervalMs);
    return () => window.clearInterval(timer);
  }, [intervalMs, enabled]);
  return now;
}
