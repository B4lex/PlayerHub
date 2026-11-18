import { useEffect, useMemo, useRef, useState } from "react";
import uPlot from "uplot";
import "uplot/dist/uPlot.min.css";
import { formatBucket } from "./format";
import styles from "./TimeChart.module.css";
import type { ChartTheme } from "./useChartTheme";

export interface ChartSeries {
  label: string;
  values: (number | null)[];
  color: string;
  /** A washed area under a line. */
  area?: boolean;
  /** A thin constant line for scale, kept out of the axis range when far above the data. */
  reference?: boolean;
}

interface TimeChartProps {
  /** Bucket starts. */
  atMs: number[];
  toMs: number;
  stepMinutes: number;
  series: ChartSeries[];
  /** Bars stack in the order given, the first at the bottom. */
  kind?: "line" | "bars";
  format: (value: number) => string;
  /** Axis labels; defaults to `format`. */
  axisFormat?: (value: number) => string;
  theme: ChartTheme;
  table: boolean;
  height?: number;
}

interface Hover {
  index: number;
  left: number;
  top: number;
  flip: boolean;
}

function bucketEnds(atMs: number[], toMs: number): number[] {
  return atMs.map((_, i) => atMs[i + 1] ?? toMs);
}

/** Runs of bars stack by drawing each running total, the tallest first, so lower parts cover it. */
function drawnSeries(series: ChartSeries[], kind: "line" | "bars"): ChartSeries[] {
  if (kind === "line") return series;
  const totals: ChartSeries[] = [];
  let running: (number | null)[] = series[0]?.values.map(() => 0) ?? [];
  for (const part of series) {
    running = running.map((sum, i) => (sum ?? 0) + (part.values[i] ?? 0));
    // A zero would draw as a hairline on the baseline.
    totals.push({ ...part, values: running.map((v) => (v === 0 ? null : v)) });
  }
  return totals.reverse();
}

/** Values with no value on either side, which a line cannot show, so they get a dot. */
function isolated(values: (number | null)[]): number[] {
  const indices: number[] = [];
  values.forEach((v, i) => {
    if (v !== null && (values[i - 1] ?? null) === null && (values[i + 1] ?? null) === null) indices.push(i);
  });
  return indices;
}

export function TimeChart({
  atMs,
  toMs,
  stepMinutes,
  series,
  kind = "line",
  format,
  axisFormat = format,
  theme,
  table,
  height = 240,
}: TimeChartProps) {
  const container = useRef<HTMLDivElement>(null);
  const [hover, setHover] = useState<Hover | null>(null);
  const ends = useMemo(() => bucketEnds(atMs, toMs), [atMs, toMs]);

  useEffect(() => {
    const element = container.current;
    if (table || element === null || atMs.length === 0) return;
    const centers = atMs.map((start, i) => (start + (ends[i] ?? toMs)) / 2);
    const drawn = drawnSeries(series, kind);
    let top = 0;
    for (const s of drawn) {
      if (s.reference) continue;
      for (const v of s.values) if (v !== null && v > top) top = v;
    }
    const reference = drawn.find((s) => s.reference)?.values[0] ?? null;
    if (reference !== null && reference <= top * 2) top = Math.max(top, reference);
    const axis = {
      stroke: theme.muted,
      font: `12px ${theme.font}`,
      grid: { stroke: theme.grid, width: 1 },
      ticks: { stroke: theme.grid, width: 1, size: 4 },
    };
    const bars = uPlot.paths.bars?.({ size: [0.72, 24], gap: 2 });
    const plot = new uPlot(
      {
        width: element.clientWidth,
        height,
        ms: 1,
        legend: { show: false },
        cursor: { y: false, drag: { x: false, y: false }, points: { show: kind === "line", size: 8, width: 2 } },
        scales: {
          x: { time: true, range: [atMs[0] ?? toMs, toMs] },
          y: { range: () => [0, top > 0 ? top * 1.1 : 1] },
        },
        axes: [axis, { ...axis, size: 70, values: (_, splits) => splits.map((v) => axisFormat(v)) }],
        series: [
          {},
          ...drawn.map((s) => ({
            label: s.label,
            stroke: s.color,
            width: s.reference ? 1 : 2,
            dash: s.reference ? [5, 4] : undefined,
            fill: kind === "bars" ? s.color : s.area ? `${s.color}1a` : undefined,
            paths: kind === "bars" ? bars : undefined,
            points:
              kind === "bars" || s.reference
                ? { show: false }
                : { show: true, size: 6, width: 0, fill: s.color, filter: isolated(s.values) },
          })),
        ],
        hooks: {
          setCursor: [
            (u) => {
              const index = u.cursor.idx;
              const left = u.cursor.left ?? -1;
              if (index === null || index === undefined || left < 0) {
                setHover(null);
                return;
              }
              setHover({
                index,
                left: u.over.offsetLeft + left,
                top: u.over.offsetTop + (u.cursor.top ?? 0),
                flip: left > u.over.clientWidth / 2,
              });
            },
          ],
        },
      },
      [centers, ...drawn.map((s) => s.values)],
      element,
    );
    const observer = new ResizeObserver(() => plot.setSize({ width: element.clientWidth, height }));
    observer.observe(element);
    return () => {
      observer.disconnect();
      plot.destroy();
      setHover(null);
    };
  }, [atMs, ends, toMs, series, kind, theme, height, axisFormat, table]);

  const shown = series.filter((s) => !s.reference);
  const legend = (
    <ul className={styles.legend}>
      {series.map((s) => (
        <li key={s.label}>
          <span
            className={kind === "bars" ? styles.swatch : s.reference ? styles.dashKey : styles.lineKey}
            style={{ color: s.color }}
          />
          {s.label}
          {s.reference && s.values[0] !== null && s.values[0] !== undefined && ` (${format(s.values[0])})`}
        </li>
      ))}
    </ul>
  );

  if (table) {
    return (
      <>
        {legend}
        <div className={styles.tableWrap}>
          <table className={styles.table}>
            <thead>
              <tr>
                <th scope="col">Time</th>
                {shown.map((s) => (
                  <th scope="col" key={s.label}>
                    {s.label}
                  </th>
                ))}
              </tr>
            </thead>
            <tbody>
              {atMs.map((start, i) => (
                <tr key={start}>
                  <th scope="row">{formatBucket(start, ends[i] ?? toMs, stepMinutes)}</th>
                  {shown.map((s) => {
                    const v = s.values[i];
                    return (
                      <td key={s.label} className="tabular">
                        {v === null || v === undefined ? "—" : format(v)}
                      </td>
                    );
                  })}
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </>
    );
  }

  return (
    <>
      {legend}
      <div className={styles.plot}>
        <div ref={container} />
        {hover !== null && (
          <div
            className={styles.tooltip}
            style={{
              left: hover.left,
              top: hover.top,
              transform: hover.flip ? "translate(calc(-100% - 12px), -50%)" : "translate(12px, -50%)",
            }}
            role="presentation"
          >
            <div className={styles.tooltipTitle}>{formatBucket(atMs[hover.index] ?? toMs, ends[hover.index] ?? toMs, stepMinutes)}</div>
            {(kind === "bars" ? [...shown].reverse() : shown).map((s) => {
              const v = s.values[hover.index];
              return (
                <div key={s.label} className={styles.tooltipRow}>
                  <span className={styles.lineKey} style={{ color: s.color }} />
                  <strong className="tabular">{v === null || v === undefined ? "—" : format(v)}</strong>
                  <span className={styles.tooltipLabel}>{s.label}</span>
                </div>
              );
            })}
          </div>
        )}
      </div>
    </>
  );
}
