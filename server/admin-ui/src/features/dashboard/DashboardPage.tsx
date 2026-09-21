import { Link } from "react-router-dom";
import type { Player } from "../../api/types";
import { Badge } from "../../components/Badge";
import { EmptyState } from "../../components/EmptyState";
import { PageHeader } from "../../components/PageHeader";
import { Panel } from "../../components/Panel";
import { StatTile } from "../../components/StatTile";
import { formatBytes, formatFormId, formatTimestamp, plural, shortId } from "../../lib/format";
import { useTicker } from "../../lib/useTicker";
import { useServerState } from "../../state/ServerState";
import styles from "./DashboardPage.module.css";

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

function PlayerRow({ player }: { player: Player }) {
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
      <td>
        <div className={styles.badges}>
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
          {!player.virtual && player.recordingId === null && <Badge tone="success">Online</Badge>}
        </div>
      </td>
    </tr>
  );
}

export function DashboardPage() {
  const { loaded, server, snapshotReceivedAt, players, recordings, playbacks } = useServerState();
  const now = useTicker(1000);

  const online = players.filter((player) => !player.virtual);
  const capturing = recordings.filter((recording) => recording.status === "recording");
  const stored = recordings.filter((recording) => recording.status !== "recording");
  const playing = playbacks.filter((playback) => playback.status === "playing");
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
                <th scope="col">Status</th>
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
    </>
  );
}
