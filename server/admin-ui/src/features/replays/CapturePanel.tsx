import { useState } from "react";
import { api } from "../../api/client";
import type { Player, Recording } from "../../api/types";
import { Badge } from "../../components/Badge";
import { Button } from "../../components/Button";
import { EmptyState } from "../../components/EmptyState";
import { Panel } from "../../components/Panel";
import { formatTimestamp, plural, shortId } from "../../lib/format";
import { useServerState } from "../../state/ServerState";
import { useAction } from "../../state/useAction";
import styles from "./CapturePanel.module.css";

interface PlayerRowProps {
  player: Player;
  capture: Recording | undefined;
  /** Label for a capture started from this row. */
  label: string;
  onStarted: () => void;
}

function PlayerRow({ player, capture, label, onStarted }: PlayerRowProps) {
  const [run, busy] = useAction();

  const start = async () => {
    if (await run(() => api.recordings.start(player.id, label))) onStarted();
  };

  return (
    <li className={styles.player}>
      <div className={styles.who}>
        <div className={styles.name}>{player.name || "Unnamed player"}</div>
        <div className={`${styles.id} mono`} title={player.id}>
          {shortId(player.id)}
        </div>
      </div>

      {capture ? (
        <>
          <div className={styles.progress}>
            <Badge tone="danger" dot pulse>
              REC
            </Badge>
            <span className="mono tabular">{formatTimestamp(capture.durationMs)}</span>
            <span className={styles.messages}>{plural(capture.messageCount, "message")}</span>
          </div>
          <Button size="sm" icon="stop" busy={busy} onClick={() => run(() => api.recordings.stop(capture.id))}>
            Stop
          </Button>
        </>
      ) : (
        <Button size="sm" icon="record" busy={busy} onClick={start} className={styles.record}>
          Record
        </Button>
      )}
    </li>
  );
}

export function CapturePanel() {
  const { loaded, players, recordings } = useServerState();
  const [label, setLabel] = useState("");

  const online = players.filter((player) => !player.virtual);
  const capturesByPlayer = new Map(
    recordings
      .filter((recording) => recording.status === "recording")
      .map((recording) => [recording.playerId, recording]),
  );

  return (
    <Panel
      title="Capture a player"
      description="Record what a connected player does until you stop it."
    >
      <div className={styles.stack}>
        <label className={styles.field}>
          <span>Label for the next recording (optional)</span>
          <input
            type="text"
            value={label}
            maxLength={100}
            placeholder="e.g. Dragon fight at Whiterun"
            onChange={(event) => setLabel(event.target.value)}
          />
        </label>

        {online.length === 0 ? (
          <EmptyState icon="users" title={loaded ? "No players connected" : "Waiting for the server…"}>
            {loaded && "Players can be recorded once the Skyrim plugin connects."}
          </EmptyState>
        ) : (
          <ul className={styles.list}>
            {online.map((player) => (
              <PlayerRow
                key={player.id}
                player={player}
                capture={player.recordingId === null ? undefined : capturesByPlayer.get(player.id)}
                label={label}
                onStarted={() => setLabel("")}
              />
            ))}
          </ul>
        )}
      </div>
    </Panel>
  );
}
