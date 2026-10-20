import { useState } from "react";
import { api } from "../../api/client";
import { Button } from "../../components/Button";
import { ConfirmButton } from "../../components/ConfirmButton";
import { EmptyState } from "../../components/EmptyState";
import { Panel } from "../../components/Panel";
import { formatDateTime } from "../../lib/format";
import { useServerState } from "../../state/ServerState";
import { useAction } from "../../state/useAction";
import styles from "./DashboardPage.module.css";

/** Bans connected players, and lists every ban until it is lifted. */
export function BansPanel() {
  const { players, bans } = useServerState();
  const [run, busy] = useAction();
  const connected = players.filter((player) => !player.virtual);
  const [playerId, setPlayerId] = useState("");
  const [withAddress, setWithAddress] = useState(false);
  const player = connected.find((candidate) => candidate.id === playerId) ?? connected[0];

  const ban = async () => {
    if (!player) return;
    const banned = await run(() => api.players.ban(player.id, withAddress && player.address !== null));
    if (banned) setWithAddress(false);
  };

  return (
    <Panel
      title="Bans"
      description="A banned session cannot connect again; an address ban also refuses everyone joining from that address."
    >
      {connected.length === 0 ? (
        bans.length === 0 && (
          <EmptyState icon="users" title="No players connected">
            Players are banned while they are connected; their bans are listed here until lifted.
          </EmptyState>
        )
      ) : (
        <div className={styles.exclusionForm}>
          <label className={styles.field}>
            Player
            <select value={player?.id ?? ""} onChange={(event) => setPlayerId(event.target.value)}>
              {connected.map((candidate) => (
                <option key={candidate.id} value={candidate.id}>
                  {candidate.name || "Unnamed player"} ({candidate.id})
                </option>
              ))}
            </select>
          </label>
          <label className={styles.check}>
            <input
              type="checkbox"
              checked={withAddress && player?.address != null}
              disabled={player?.address == null}
              onChange={(event) => setWithAddress(event.target.checked)}
            />
            <span>Also ban the address{player?.address ? ` ${player.address}` : ""}</span>
          </label>
          <ConfirmButton variant="danger" busy={busy} disabled={!player} confirmLabel="Confirm ban" onConfirm={ban}>
            Ban
          </ConfirmButton>
        </div>
      )}
      {bans.length > 0 && (
        <ul className={styles.exclusions}>
          {bans.map((entry) => (
            <li key={entry.id}>
              <span>
                <strong>{entry.displayName}</strong>
                <span className={styles.muted}>
                  {" "}
                  · session {entry.sessionId}
                  {entry.address !== null && ` · address ${entry.address}`} · since{" "}
                  {formatDateTime(new Date(entry.createdAtMs).toISOString())}
                </span>
              </span>
              <Button size="sm" variant="ghost" icon="close" busy={busy} onClick={() => run(() => api.bans.lift(entry.id))}>
                Lift
              </Button>
            </li>
          ))}
        </ul>
      )}
    </Panel>
  );
}
