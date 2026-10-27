import { useState } from "react";
import { Link } from "react-router-dom";
import { api } from "../../api/client";
import type { Player } from "../../api/types";
import { Badge } from "../../components/Badge";
import { ConfirmDialog } from "../../components/ConfirmDialog";
import { EmptyState } from "../../components/EmptyState";
import { Menu } from "../../components/Menu";
import { PageHeader } from "../../components/PageHeader";
import { Panel } from "../../components/Panel";
import { StatTile } from "../../components/StatTile";
import { formatBytes, formatFormRef, formatTimestamp, plural, shortId } from "../../lib/format";
import { useTicker } from "../../lib/useTicker";
import { useServerState } from "../../state/ServerState";
import { useAction } from "../../state/useAction";
import styles from "./DashboardPage.module.css";

/** "sneakingIdle" -> "sneaking idle". */
function humanize(name: string): string {
  return name.replace(/([a-z])([A-Z])/g, "$1 $2").toLowerCase();
}

function describeLocation(player: Player): string {
  if (player.worldspace !== null) return `Worldspace ${formatFormRef(player.worldspace)}`;
  if (player.interiorCell !== null) return `Interior ${formatFormRef(player.interiorCell)}`;
  return "—";
}

function describeTraffic(player: Player): string {
  const kilobytes = player.bytesPerSecond / 1000;
  return `${kilobytes < 10 ? kilobytes.toFixed(1) : Math.round(kilobytes)} KB/s`;
}

type PlayerCommand = "hide" | "show" | "kick" | "ban";

/** The row's command menu; every command asks for confirmation first. */
function PlayerActions({ player }: { player: Player }) {
  const [run, busy] = useAction();
  const [pending, setPending] = useState<PlayerCommand | null>(null);
  const [withAddress, setWithAddress] = useState(false);
  const name = player.name || "this player";

  const ask = (command: PlayerCommand) => {
    setWithAddress(false);
    setPending(command);
  };

  const confirm = async () => {
    const commands: Record<PlayerCommand, () => Promise<unknown>> = {
      hide: () => api.players.setHidden(player.id, true),
      show: () => api.players.setHidden(player.id, false),
      kick: () => api.players.kick(player.id),
      ban: () => api.players.ban(player.id, withAddress && player.address !== null),
    };
    if (pending === null) return;
    await run(commands[pending]);
    setPending(null);
  };

  const close = () => setPending(null);

  return (
    <>
      <Menu
        label={`Actions for ${player.name || `player ${player.id}`}`}
        items={[
          player.hidden
            ? { label: "Show to everyone", onSelect: () => ask("show") }
            : { label: "Hide from everyone", onSelect: () => ask("hide") },
          { label: "Kick", danger: true, onSelect: () => ask("kick") },
          { label: "Ban", danger: true, onSelect: () => ask("ban") },
        ]}
      />
      {pending === "hide" && (
        <ConfirmDialog title={`Hide ${name}?`} confirmLabel="Hide" busy={busy} onConfirm={confirm} onClose={close}>
          <p>No other player sees them until they are shown again or disconnect.</p>
        </ConfirmDialog>
      )}
      {pending === "show" && (
        <ConfirmDialog title={`Show ${name}?`} confirmLabel="Show" busy={busy} onConfirm={confirm} onClose={close}>
          <p>Other players see them again.</p>
        </ConfirmDialog>
      )}
      {pending === "kick" && (
        <ConfirmDialog title={`Kick ${name}?`} confirmLabel="Kick" danger busy={busy} onConfirm={confirm} onClose={close}>
          <p>They are disconnected, and their game waits for them to connect again.</p>
        </ConfirmDialog>
      )}
      {pending === "ban" && (
        <ConfirmDialog title={`Ban ${name}?`} confirmLabel="Ban" danger busy={busy} onConfirm={confirm} onClose={close}>
          <p>
            They are disconnected and their session cannot connect again. Deleting the session on the{" "}
            <Link to="/sessions">Sessions</Link> page lifts the ban.
          </p>
          <label className={styles.check}>
            <input
              type="checkbox"
              checked={withAddress && player.address !== null}
              disabled={player.address === null || busy}
              onChange={(event) => setWithAddress(event.target.checked)}
            />
            <span>Also refuse everyone connecting from their address</span>
          </label>
        </ConfirmDialog>
      )}
    </>
  );
}

function PlayerRow({ player }: { player: Player }) {
  const location = describeLocation(player);
  const doing = player.movementState ? humanize(player.movementState) : "—";
  return (
    <tr>
      <td className={styles.menuCell}>{!player.virtual && <PlayerActions player={player} />}</td>
      <td>
        <div className={styles.name} title={player.name || undefined}>
          {player.name || <span className={styles.muted}>Unnamed player</span>}
        </div>
        <div className={`${styles.id} mono`} title={player.id}>
          {player.virtual ? player.id : shortId(player.id)}
        </div>
      </td>
      <td title={location}>{location}</td>
      <td className={styles.muted} title={doing}>
        {doing}
      </td>
      <td className="tabular">
        {player.virtual ? (
          "—"
        ) : (
          <>
            <div title="Peers this player receives">{plural(player.visible, "peer")}</div>
            <div className={styles.muted}>{describeTraffic(player)}</div>
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
            <colgroup>
              <col className={styles.menuColumn} />
              <col className={styles.playerColumn} />
              <col />
              <col className={styles.doingColumn} />
              <col className={styles.receivesColumn} />
              <col className={styles.statusColumn} />
            </colgroup>
            <thead>
              <tr>
                <th scope="col">
                  <span className="visually-hidden">Actions</span>
                </th>
                <th scope="col">Player</th>
                <th scope="col">Location</th>
                <th scope="col">Doing</th>
                <th scope="col">Receives</th>
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
