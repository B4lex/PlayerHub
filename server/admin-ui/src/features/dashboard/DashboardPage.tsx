import { Link } from "react-router-dom";
import { api } from "../../api/client";
import type { Player } from "../../api/types";
import { Badge } from "../../components/Badge";
import { Button } from "../../components/Button";
import { EmptyState } from "../../components/EmptyState";
import { PageHeader } from "../../components/PageHeader";
import { Panel } from "../../components/Panel";
import { StatTile } from "../../components/StatTile";
import { formatBytes, formatFormId, formatTimestamp, plural, shortId } from "../../lib/format";
import { useTicker } from "../../lib/useTicker";
import { useServerState } from "../../state/ServerState";
import { useAction } from "../../state/useAction";
import styles from "./DashboardPage.module.css";
import { ExclusionsPanel } from "./ExclusionsPanel";

/** "sneakingIdle" -> "sneaking idle". */
function humanize(name: string): string {
  return name.replace(/([a-z])([A-Z])/g, "$1 $2").toLowerCase();
}

function describeLocation(player: Player): string {
  if (player.cellId === null || player.worldspaceId === null) return "—";
  return player.worldspaceId !== 0
    ? `Worldspace ${formatFormId(player.worldspaceId)}`
    : `Interior ${formatFormId(player.cellId)}`;
}

function describePosition(player: Player): string {
  if (player.position === null) return "—";
  const { x, y, z } = player.position;
  return [x, y, z].map((value) => Math.round(value)).join(", ");
}

function describeTraffic(player: Player): string {
  if (player.virtual) return "—";
  const kilobytes = player.bytesPerSecond / 1000;
  return `${kilobytes < 10 ? kilobytes.toFixed(1) : Math.round(kilobytes)} KB/s`;
}

function PlayerRow({ player }: { player: Player }) {
  const [run, busy] = useAction();
  return (
    <tr>
      <td>
        <div className={styles.name}>{player.name || <span className={styles.muted}>Unnamed player</span>}</div>
        <div className={`${styles.id} mono`} title={player.id}>
          {player.virtual ? player.id : shortId(player.id)}
        </div>
      </td>
      <td>{describeLocation(player)}</td>
      <td className={`${styles.position} mono tabular`}>{describePosition(player)}</td>
      <td className={styles.muted}>{player.movementState ? humanize(player.movementState) : "—"}</td>
      <td className="tabular">
        {player.virtual ? (
          "—"
        ) : (
          <>
            <div title="Peers this player receives">{plural(player.visible, "peer")}</div>
            <div
              className={styles.muted}
              title={`${player.replacedPerSecond.toFixed(1)} checkpoints/s replaced before they were sent`}
            >
              {describeTraffic(player)}
              {player.replacedPerSecond >= 1 && ` · ${Math.round(player.replacedPerSecond)} dropped/s`}
            </div>
          </>
        )}
      </td>
      <td>
        <div className={styles.badges}>
          {player.hidden && (
            <Badge tone="neutral" title="Hidden from every other player">
              Hidden
            </Badge>
          )}
          {player.virtual && (
            <Badge tone="info" title="A replay's virtual player">
              Replay
            </Badge>
          )}
          {player.recordingId !== null && (
            <Badge tone="danger" dot pulse>
              Recording
            </Badge>
          )}
          {!player.virtual && player.recordingId === null && player.inWorld && <Badge tone="success">Online</Badge>}
          {!player.inWorld && (
            <Badge tone="neutral" title="In the main menu or a loading screen">
              Out of world
            </Badge>
          )}
        </div>
      </td>
      <td>
        {!player.virtual && (
          <Button size="sm" variant="ghost" busy={busy} onClick={() => run(() => api.players.setHidden(player.id, !player.hidden))}>
            {player.hidden ? "Show" : "Hide"}
          </Button>
        )}
      </td>
    </tr>
  );
}

export function DashboardPage() {
  const { loaded, server, snapshotReceivedAt, players, recordings, playbacks, batches } = useServerState();
  const now = useTicker(1000);

  const online = players.filter((player) => !player.virtual);
  const capturing = recordings.filter((recording) => recording.status === "recording");
  const stored = recordings.filter((recording) => recording.status !== "recording");
  const playing = playbacks.filter((playback) => playback.status === "playing");
  const bots = batches.reduce((total, batch) => total + batch.bots.filter((bot) => bot.playerId !== null).length, 0);
  const storedBytes = stored.reduce((total, recording) => total + recording.sizeBytes, 0);
  // The server's own clock, carried forward locally, so it is right even if this browser's clock is not.
  const uptimeMs =
    server && snapshotReceivedAt !== null
      ? server.serverTimeMs - server.startedAtMs + Math.max(0, now - snapshotReceivedAt)
      : null;

  return (
    <>
      <PageHeader title="Dashboard" description="What is happening on the server right now." />

      <div className={styles.tiles}>
        <StatTile icon="users" label="Players online" value={loaded ? online.length : "–"} />
        <StatTile
          icon="record"
          label="Recording now"
          value={loaded ? capturing.length : "–"}
          hint={capturing.length > 0 ? capturing.map((recording) => recording.playerName || "player").join(", ") : "Nothing being captured"}
          live={capturing.length > 0}
        />
        <StatTile
          icon="replay"
          label="Active replays"
          value={loaded ? playbacks.length : "–"}
          hint={playbacks.length > 0 ? `${playing.length} playing, ${playbacks.length - playing.length} paused or finished` : "None running"}
        />
        <StatTile
          icon="gauge"
          label="Load-test bots"
          value={loaded ? bots : "–"}
          hint={batches.length > 0 ? <Link to="/load-testing">{plural(batches.length, "batch", "batches")}</Link> : "No batches running"}
        />
        <StatTile
          icon="disk"
          label="Stored recordings"
          value={loaded ? stored.length : "–"}
          hint={loaded ? formatBytes(storedBytes) : undefined}
        />
        <StatTile icon="clock" label="Server uptime" value={uptimeMs === null ? "–" : formatTimestamp(uptimeMs)} />
      </div>

      <Panel
        title="Players"
        description={loaded ? plural(online.length, "player") + " connected" : "Waiting for the server…"}
        flush
      >
        {players.length === 0 ? (
          <EmptyState icon="users" title="No players connected">
            Players appear here as soon as the Skyrim plugin connects to this server. You can{" "}
            <Link to="/replays">replay a recording</Link> to put a virtual player in the world meanwhile.
          </EmptyState>
        ) : (
          <table className={styles.table}>
            <thead>
              <tr>
                <th scope="col">Player</th>
                <th scope="col">Location</th>
                <th scope="col">Position</th>
                <th scope="col">Doing</th>
                <th scope="col">Receives</th>
                <th scope="col">Status</th>
                <th scope="col">
                  <span className="visually-hidden">Actions</span>
                </th>
              </tr>
            </thead>
            <tbody>
              {players.map((player) => (
                <PlayerRow key={player.id} player={player} />
              ))}
            </tbody>
          </table>
        )}
      </Panel>

      <div className={styles.section}>
        <ExclusionsPanel />
      </div>
    </>
  );
}
