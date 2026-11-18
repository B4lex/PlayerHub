import { memo, useEffect, useMemo, useState, type ReactNode } from "react";
import { Link, useSearchParams } from "react-router-dom";
import { api } from "../../api/client";
import type { ReturnShare, Statistics, StatisticsRange } from "../../api/types";
import { Button } from "../../components/Button";
import { EmptyState } from "../../components/EmptyState";
import { PageHeader } from "../../components/PageHeader";
import { Panel } from "../../components/Panel";
import { Spinner } from "../../components/Spinner";
import { StatTile } from "../../components/StatTile";
import { useServerState } from "../../state/ServerState";
import { BarList } from "./BarList";
import {
  disconnectLabel,
  formatBandwidth,
  formatCount,
  formatDuration,
  formatMoment,
  formatPercent,
  refusalLabel,
} from "./format";
import { Heatmap } from "./Heatmap";
import styles from "./StatisticsPage.module.css";
import { TimeChart, type ChartSeries } from "./TimeChart";
import { useChartTheme } from "./useChartTheme";

const ranges: { id: StatisticsRange; label: string }[] = [
  { id: "24h", label: "24 hours" },
  { id: "7d", label: "7 days" },
  { id: "30d", label: "30 days" },
  { id: "90d", label: "90 days" },
  { id: "all", label: "All" },
];

const REFRESH_MS = 60_000;
const timeZone = Intl.DateTimeFormat().resolvedOptions().timeZone;

const formatMilliseconds = (ms: number) => `${ms.toFixed(ms < 10 ? 1 : 0)} ms`;

function isRange(value: string | null): value is StatisticsRange {
  return ranges.some((range) => range.id === value);
}

function shareValue(share: ReturnShare): string {
  return share.eligible === 0 ? "–" : formatPercent(share.returned / share.eligible);
}

function shareHint(share: ReturnShare): string {
  return share.eligible === 0
    ? "No new player's period has passed yet"
    : `${share.returned} of ${share.eligible} new players`;
}

function lengthLabel(maxSeconds: number | null, previous: number | null): string {
  if (maxSeconds === null) return `Over ${formatDuration(previous ?? 0)}`;
  if (previous === null) return `Under ${formatDuration(maxSeconds)}`;
  return `${formatDuration(previous)} – ${formatDuration(maxSeconds)}`;
}

/** A panel whose chart can be swapped for its data as a table. */
function ChartPanel({
  title,
  description,
  children,
}: {
  title: string;
  description?: ReactNode;
  children: (table: boolean) => ReactNode;
}) {
  const [table, setTable] = useState(false);
  return (
    <Panel
      title={title}
      description={description}
      actions={
        <Button size="sm" variant="ghost" onClick={() => setTable((shown) => !shown)} aria-pressed={table}>
          {table ? "Chart" : "Table"}
        </Button>
      }
    >
      {children(table)}
    </Panel>
  );
}

function useStatistics(range: StatisticsRange) {
  const [statistics, setStatistics] = useState<Statistics | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [loading, setLoading] = useState(true);

  useEffect(() => {
    let current = true;
    const load = () => {
      setLoading(true);
      api.statistics.get(range, timeZone).then(
        (loaded) => {
          if (!current) return;
          setStatistics(loaded);
          setError(null);
          setLoading(false);
        },
        (failure: unknown) => {
          if (!current) return;
          setError(failure instanceof Error ? failure.message : String(failure));
          setLoading(false);
        },
      );
    };
    load();
    const timer = window.setInterval(load, REFRESH_MS);
    return () => {
      current = false;
      window.clearInterval(timer);
    };
  }, [range]);

  return { statistics, error, loading };
}

// Memoized: the page re-renders with every live snapshot, and a chart is rebuilt when its props change.
const Charts = memo(function Charts({ statistics }: { statistics: Statistics }) {
  const theme = useChartTheme();
  const [blue, orange, aqua] = theme.series;
  const { fine, coarse, summary } = statistics;
  const daily = coarse.stepMinutes >= 1440;

  const online = useMemo<ChartSeries[]>(
    () => [
      { label: "Peak", values: fine.playersMax, color: orange },
      { label: "Average", values: fine.playersAvg, color: blue, area: true },
      { label: "In the world", values: fine.inWorldAvg, color: aqua },
    ],
    [fine, blue, orange, aqua],
  );
  const players = useMemo<ChartSeries[]>(
    () => [
      { label: "New", values: coarse.new, color: blue },
      { label: "Returning", values: coarse.returning, color: orange },
    ],
    [coarse, blue, orange],
  );
  const bandwidth = useMemo<ChartSeries[]>(() => {
    const series: ChartSeries[] = [
      { label: "Peak minute out", values: fine.bytesOutPeak, color: orange },
      { label: "Average out", values: fine.bytesOutAvg, color: blue, area: true },
      { label: "Average in", values: fine.bytesInAvg, color: aqua },
    ];
    if (statistics.uplinkBytesPerSecond !== null) {
      const uplink = statistics.uplinkBytesPerSecond;
      series.push({ label: "Uplink", values: fine.atMs.map(() => uplink), color: theme.faint, reference: true });
    }
    return series;
  }, [fine, statistics.uplinkBytesPerSecond, blue, orange, aqua, theme.faint]);
  const loopBusy = useMemo<ChartSeries[]>(() => [{ label: "Busy", values: fine.loopBusy, color: blue }], [fine, blue]);
  const loopWorst = useMemo<ChartSeries[]>(
    () => [{ label: "Longest pass", values: fine.loopWorstMs, color: blue }],
    [fine, blue],
  );
  const refusals = useMemo<ChartSeries[]>(
    () => [
      {
        label: "Refused",
        values: coarse.atMs.map((_, i) => statistics.refusals.reduce((sum, reason) => sum + (reason.counts[i] ?? 0), 0)),
        color: blue,
      },
    ],
    [coarse, statistics.refusals, blue],
  );

  const chart = { toMs: statistics.toMs, theme };
  const fineChart = { ...chart, atMs: fine.atMs, stepMinutes: fine.stepMinutes };
  const coarseChart = { ...chart, atMs: coarse.atMs, stepMinutes: coarse.stepMinutes };

  return (
    <div className={styles.stack}>
      <ChartPanel title="Players online" description="Connected players per interval; peak is the busiest minute.">
        {(table) => <TimeChart {...fineChart} series={online} format={formatCount} table={table} />}
      </ChartPanel>

      <ChartPanel
        title={daily ? "Players per day" : "Players per hour"}
        description="Different players who connected, split by whether this was their first time."
      >
        {(table) => <TimeChart {...coarseChart} kind="bars" series={players} format={formatCount} table={table} />}
      </ChartPanel>

      <div className={styles.columns}>
        <Panel title="When people play" description={`Average players by weekday and hour (${timeZone}).`}>
          <Heatmap values={statistics.heatmap} />
        </Panel>
        <Panel
          title="Play session length"
          description={`Connections closer than 5 minutes count as one session; ${summary.playSessions.toLocaleString()} finished in the range.`}
        >
          {summary.playSessions === 0 ? (
            <p className={styles.muted}>No finished play sessions in this range.</p>
          ) : (
            <BarList
              items={statistics.sessionLengths.map((bucket, i) => ({
                key: String(i),
                label: lengthLabel(bucket.maxSeconds, statistics.sessionLengths[i - 1]?.maxSeconds ?? null),
                value: bucket.count,
              }))}
            />
          )}
        </Panel>
      </div>

      <Panel title="Most active players" description="Time connected in this range." flush>
        {statistics.topPlayers.length === 0 ? (
          <EmptyState icon="users" title="Nobody played in this range" />
        ) : (
          <table className={styles.table}>
            <thead>
              <tr>
                <th scope="col">Player</th>
                <th scope="col" className={styles.number}>
                  Play time
                </th>
                <th scope="col" className={styles.number}>
                  Connections
                </th>
              </tr>
            </thead>
            <tbody>
              {statistics.topPlayers.map((player) => (
                <tr key={player.sessionId}>
                  <td>
                    <Link to={`/sessions?search=${encodeURIComponent(player.sessionId)}`}>{player.displayName}</Link>
                    <span className={`${styles.id} mono`}> {player.sessionId}</span>
                  </td>
                  <td className={`${styles.number} tabular`}>{formatDuration(player.playSeconds)}</td>
                  <td className={`${styles.number} tabular`}>{player.connections.toLocaleString()}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </Panel>

      <h2 className={styles.section}>Server health</h2>

      <ChartPanel
        title="Bandwidth"
        description={
          statistics.uplinkBytesPerSecond === null
            ? "WebSocket traffic, without voice. The budget is off, so no uplink is shown."
            : "WebSocket traffic, without voice, against the budget's uplink."
        }
      >
        {(table) => <TimeChart {...fineChart} series={bandwidth} format={formatBandwidth} table={table} />}
      </ChartPanel>

      <div className={styles.columns}>
        <ChartPanel title="Hub loop load" description="Share of the time the hub loop was working.">
          {(table) => (
            <TimeChart {...fineChart} series={loopBusy} format={formatPercent} table={table} height={200} />
          )}
        </ChartPanel>
        <ChartPanel title="Longest hub pass" description="The slowest single pass in each interval.">
          {(table) => (
            <TimeChart
              {...fineChart}
              series={loopWorst}
              format={formatMilliseconds}
              table={table}
              height={200}
            />
          )}
        </ChartPanel>
      </div>

      <div className={styles.columns}>
        <ChartPanel title="Refused joins" description="Connections turned away before joining.">
          {(table) => (
            <>
              <TimeChart {...coarseChart} kind="bars" series={refusals} format={formatCount} table={table} height={160} />
              <div className={styles.reasons}>
                {statistics.refusals.length === 0 ? (
                  <p className={styles.muted}>No refusals in this range.</p>
                ) : (
                  <BarList
                    items={statistics.refusals.map((r) => ({ key: r.reason, label: refusalLabel(r.reason), value: r.total }))}
                  />
                )}
              </div>
            </>
          )}
        </ChartPanel>
        <Panel title="Disconnects" description="Why connections ended in this range.">
          {statistics.disconnects.length === 0 ? (
            <p className={styles.muted}>No connection ended in this range.</p>
          ) : (
            <BarList
              items={statistics.disconnects.map((d) => ({ key: d.reason, label: disconnectLabel(d.reason), value: d.count }))}
            />
          )}
        </Panel>
      </div>
    </div>
  );
});

/** The server's history: players, how they play, and how the server held up. */
export function StatisticsPage() {
  const [parameters, setParameters] = useSearchParams();
  const requested = parameters.get("range");
  const range: StatisticsRange = isRange(requested) ? requested : "7d";
  const { statistics, error, loading } = useStatistics(range);
  const { loaded, players } = useServerState();
  const onlineNow = players.filter((player) => !player.virtual).length;
  const summary = statistics?.summary;

  return (
    <>
      <PageHeader title="Statistics" description="How the server has been used over time." />

      <div className={styles.toolbar}>
        <div className={styles.ranges} role="group" aria-label="Range">
          {ranges.map((option) => (
            <button
              key={option.id}
              type="button"
              className={`${styles.range} ${option.id === range ? styles.selected : ""}`}
              aria-pressed={option.id === range}
              onClick={() => setParameters(option.id === "7d" ? {} : { range: option.id }, { replace: true })}
            >
              {option.label}
            </button>
          ))}
        </div>
        {loading && statistics !== null && <Spinner />}
        {error !== null && statistics !== null && <span className={styles.error}>{error}</span>}
      </div>

      {statistics === null || summary === undefined ? (
        error !== null ? (
          <EmptyState icon="alert" title="Cannot load the statistics">
            {error}
          </EmptyState>
        ) : (
          <div className={styles.loading}>
            <Spinner />
          </div>
        )
      ) : (
        <div className={loading ? styles.refreshing : undefined}>
          <div className={styles.tiles}>
            <StatTile icon="users" label="Online now" value={loaded ? onlineNow : "–"} live={onlineNow > 0} />
            <StatTile
              icon="gauge"
              label="Peak online"
              value={summary.peakPlayers}
              hint={summary.peakAtMs === null ? "No samples yet" : formatMoment(summary.peakAtMs)}
            />
            <StatTile
              icon="users"
              label="Players"
              value={summary.uniquePlayers.toLocaleString()}
              hint={`${summary.newPlayers.toLocaleString()} new · ${summary.returningPlayers.toLocaleString()} returning`}
            />
            <StatTile
              icon="clock"
              label="Play time"
              value={formatDuration(summary.playSeconds)}
              hint={`Median session ${summary.medianSessionSeconds === null ? "–" : formatDuration(summary.medianSessionSeconds)}`}
            />
            <StatTile
              icon="restart"
              label="Came back next day"
              value={shareValue(summary.returnedNextDay)}
              hint={shareHint(summary.returnedNextDay)}
            />
            <StatTile
              icon="restart"
              label="Came back within a week"
              value={shareValue(summary.returnedWithinWeek)}
              hint={shareHint(summary.returnedWithinWeek)}
            />
          </div>
          <Charts statistics={statistics} />
        </div>
      )}
    </>
  );
}
