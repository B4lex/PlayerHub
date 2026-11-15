import { useRef, useState } from "react";
import type { Marker } from "../../api/types";
import { formatTimestamp } from "../../lib/format";
import styles from "./Scrubber.module.css";

interface ScrubberProps {
  durationMs: number;
  positionMs: number;
  markers: readonly Marker[];
  /** Move the replay; the position passed in follows at once. */
  onSeek: (positionMs: number) => void;
}

export function Scrubber({ durationMs, positionMs, markers, onSeek }: ScrubberProps) {
  // While the handle is being moved, show where it is instead of where the replay still is.
  const [scrub, setScrub] = useState<number | null>(null);
  const dragging = useRef(false);

  const shown = Math.min(scrub ?? positionMs, durationMs);
  const progress = durationMs > 0 ? (shown / durationMs) * 100 : 0;

  const commit = (target: number) => {
    onSeek(target);
    setScrub(null);
  };

  return (
    <div className={styles.scrubber}>
      <div className={styles.markers}>
        {markers.map((marker, index) => (
          <button
            key={`${marker.tMs}-${marker.kind}-${index}`}
            type="button"
            className={styles.marker}
            style={{
              left: `${durationMs > 0 ? (marker.tMs / durationMs) * 100 : 0}%`,
              background: `var(--marker-${marker.kind})`,
            }}
            title={`${marker.label} · ${formatTimestamp(marker.tMs, true)}`}
            aria-label={`Jump to ${marker.label} at ${formatTimestamp(marker.tMs, true)}`}
            onClick={() => commit(marker.tMs)}
          />
        ))}
      </div>
      <input
        className={styles.range}
        type="range"
        min={0}
        max={durationMs}
        step={10}
        value={Math.round(shown)}
        style={{ "--progress": `${progress}%` } as React.CSSProperties}
        aria-label="Timeline position"
        aria-valuetext={`${formatTimestamp(shown, true)} of ${formatTimestamp(durationMs, true)}`}
        onChange={(event) => {
          const value = Number(event.target.value);
          setScrub(value);
          // Dragging jumps once, on release; the keyboard jumps on every step.
          if (!dragging.current) commit(value);
        }}
        onPointerDown={() => {
          dragging.current = true;
        }}
        onPointerUp={(event) => {
          dragging.current = false;
          commit(Number(event.currentTarget.value));
        }}
        onPointerCancel={() => {
          dragging.current = false;
          setScrub(null);
        }}
      />
    </div>
  );
}
