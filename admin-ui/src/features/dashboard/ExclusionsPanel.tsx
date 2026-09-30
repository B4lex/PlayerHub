import { useState, type FormEvent } from "react";
import { api } from "../../api/client";
import type { Exclusion, Player } from "../../api/types";
import { Button } from "../../components/Button";
import { EmptyState } from "../../components/EmptyState";
import { Panel } from "../../components/Panel";
import { useServerState } from "../../state/ServerState";
import { useAction } from "../../state/useAction";
import styles from "./DashboardPage.module.css";

function label(players: Player[], id: string): string {
  const player = players.find((candidate) => candidate.id === id);
  return player ? `${player.name || "Unnamed player"} (${id})` : `Player ${id}`;
}

/** Directed exclusions: the viewer never receives the target. They take effect at once and end with either connection. */
export function ExclusionsPanel() {
  const { players, exclusions } = useServerState();
  const [run, busy] = useAction();
  const connected = players.filter((player) => !player.virtual);
  const [viewerId, setViewerId] = useState("");
  const [targetId, setTargetId] = useState("");
  const viewer = connected.find((player) => player.id === viewerId)?.id ?? connected[0]?.id ?? "";
  const target = connected.find((player) => player.id === targetId && player.id !== viewer)?.id
    ?? connected.find((player) => player.id !== viewer)?.id
    ?? "";

  const add = async (event: FormEvent) => {
    event.preventDefault();
    if (viewer && target) await run(() => api.exclusions.add({ viewerId: viewer, targetId: target }));
  };
  const remove = (exclusion: Exclusion) => run(() => api.exclusions.remove(exclusion));

  return (
    <Panel title="Exclusions" description="A viewer never receives its target, whatever the distance.">
      {connected.length < 2 ? (
        <EmptyState icon="users" title="Needs two connected players">
          Exclusions are between connected players and end when either disconnects.
        </EmptyState>
      ) : (
        <form className={styles.exclusionForm} onSubmit={add}>
          <label className={styles.field}>
            Viewer
            <select value={viewer} onChange={(event) => setViewerId(event.target.value)}>
              {connected.map((player) => (
                <option key={player.id} value={player.id}>
                  {label(players, player.id)}
                </option>
              ))}
            </select>
          </label>
          <label className={styles.field}>
            Never sees
            <select value={target} onChange={(event) => setTargetId(event.target.value)}>
              {connected
                .filter((player) => player.id !== viewer)
                .map((player) => (
                  <option key={player.id} value={player.id}>
                    {label(players, player.id)}
                  </option>
                ))}
            </select>
          </label>
          <Button type="submit" variant="primary" busy={busy} disabled={!viewer || !target}>
            Exclude
          </Button>
        </form>
      )}
      {exclusions.length > 0 && (
        <ul className={styles.exclusions}>
          {exclusions.map((exclusion) => (
            <li key={`${exclusion.viewerId}-${exclusion.targetId}`}>
              <span>
                {label(players, exclusion.viewerId)} never sees {label(players, exclusion.targetId)}
              </span>
              <Button size="sm" variant="ghost" icon="close" onClick={() => remove(exclusion)}>
                Lift
              </Button>
            </li>
          ))}
        </ul>
      )}
    </Panel>
  );
}
