import { useState } from "react";
import { formatCount } from "./format";
import styles from "./Heatmap.module.css";

const hours = Array.from({ length: 24 }, (_, hour) => hour);

// 2 January 2023 was a Monday; the server's rows start on Monday.
function weekdayName(day: number, width: "short" | "long"): string {
  return new Date(2023, 0, 2 + day).toLocaleDateString(undefined, { weekday: width });
}

interface Cell {
  day: number;
  hour: number;
  value: number | null;
}

function describe(cell: Cell): string {
  const when = `${weekdayName(cell.day, "long")} ${String(cell.hour).padStart(2, "0")}:00`;
  return cell.value === null ? `${when}: no data` : `${when}: ${formatCount(Math.round(cell.value * 10) / 10)} players on average`;
}

/** Average players by weekday and hour, on one blue ramp from the surface up. */
export function Heatmap({ values }: { values: (number | null)[][] }) {
  let peak: Cell | null = null;
  for (let day = 0; day < values.length; day++) {
    for (const hour of hours) {
      const value = values[day]?.[hour] ?? null;
      if (value !== null && (peak === null || peak.value === null || value > peak.value)) peak = { day, hour, value };
    }
  }
  const [hovered, setHovered] = useState<Cell | null>(null);
  const top = peak?.value ?? 0;
  const shown = hovered ?? peak;

  return (
    <div>
      <div className={styles.grid} role="table" aria-label="Average players by weekday and hour">
        <span />
        {hours.map((hour) => (
          <span key={hour} className={styles.hour} aria-hidden="true">
            {hour % 3 === 0 ? String(hour).padStart(2, "0") : ""}
          </span>
        ))}
        {values.map((row, day) => (
          <div key={day} className={styles.row} role="row">
            <span className={styles.day} role="rowheader">
              {weekdayName(day, "short")}
            </span>
            {hours.map((hour) => {
              const value = row[hour] ?? null;
              const cell = { day, hour, value };
              const share = value === null || top <= 0 ? 0 : value / top;
              return (
                <span
                  key={hour}
                  role="cell"
                  tabIndex={0}
                  aria-label={describe(cell)}
                  className={`${styles.cell} ${value === null ? styles.empty : ""}`}
                  style={value === null ? undefined : { background: `color-mix(in oklab, var(--heat-high) ${Math.round(share * 100)}%, var(--heat-low))` }}
                  onPointerEnter={() => setHovered(cell)}
                  onPointerLeave={() => setHovered(null)}
                  onFocus={() => setHovered(cell)}
                  onBlur={() => setHovered(null)}
                />
              );
            })}
          </div>
        ))}
      </div>
      <p className={styles.readout} aria-live="polite">
        {shown === null ? "No samples in this range yet." : `${hovered === null ? "Busiest: " : ""}${describe(shown)}`}
      </p>
    </div>
  );
}
