import { api } from "../../api/client";
import type { Recording } from "../../api/types";
import { Badge } from "../../components/Badge";
import { Button } from "../../components/Button";
import { ConfirmButton } from "../../components/ConfirmButton";
import { EmptyState } from "../../components/EmptyState";
import { Panel } from "../../components/Panel";
import { formatBytes, formatDateTime, formatRelative, formatTimestamp, plural } from "../../lib/format";
import { useTicker } from "../../lib/useTicker";
import { useServerState } from "../../state/ServerState";
import { useAction } from "../../state/useAction";
import styles from "./RecordingsPanel.module.css";

const endReasons: Record<string, string> = {
  stopped: "Stopped from the panel",
  disconnected: "The player disconnected",
  shutdown: "The server shut down",
  imported: "Imported from an older recording",
};

function StatusBadge({ recording }: { recording: Recording }) {
  switch (recording.status) {
    case "recording":
      return (
        <Badge tone="danger" dot pulse>
          Recording
        </Badge>
      );
    case "interrupted":
      return (
        <Badge tone="warning" title="The capture ended without being closed, for example when the server crashed. What was written is still playable.">
          Interrupted
        </Badge>
      );
    case "complete":
      return (
        <Badge tone="neutral" title={recording.endReason ? (endReasons[recording.endReason] ?? recording.endReason) : undefined}>
          Saved
        </Badge>
      );
  }
}

function RecordingRow({ recording, now }: { recording: Recording; now: number }) {
  const { adoptPlayback } = useServerState();
  const [run, playing] = useAction();
  const [runDelete, deleting] = useAction();
  const live = recording.status === "recording";

  const play = async () => {
    const playback = await run(() => api.playbacks.open({ recordingId: recording.id }));
    if (playback) adoptPlayback(playback);
  };

  return (
    <tr>
      <td>
        <div className={styles.label}>{recording.label}</div>
        <div className={styles.sub}>
          {recording.playerName || "Unnamed player"} ·{" "}
          <time dateTime={recording.startedAt} title={formatDateTime(recording.startedAt)}>
            {formatRelative(recording.startedAt, now)}
          </time>
        </div>
      </td>
      <td className={styles.numeric} title={plural(recording.messageCount, "message")}>
        <span className="mono tabular">{formatTimestamp(recording.durationMs)}</span>
        <div className={styles.sub}>{formatBytes(recording.sizeBytes)}</div>
      </td>
      <td>
        <StatusBadge recording={recording} />
      </td>
      <td>
        <div className={styles.actions}>
          <Button
            variant="primary"
            size="sm"
            icon="play"
            busy={playing}
            disabled={live}
            onClick={play}
            title={live ? "Stop the capture before replaying it" : "Spawn this recording as a virtual player for everyone connected"}
          >
            Play
          </Button>
          <ConfirmButton
            variant="ghost"
            size="sm"
            icon="trash"
            busy={deleting}
            disabled={live}
            confirmLabel="Really delete?"
            aria-label={`Delete ${recording.label}`}
            title={live ? "Stop the capture before deleting it" : "Delete this recording"}
            onConfirm={() => runDelete(() => api.recordings.remove(recording.id))}
          />
        </div>
      </td>
    </tr>
  );
}

export function RecordingsPanel() {
  const { loaded, recordings } = useServerState();
  useTicker(30_000); // re-render so relative times ("5 min ago") stay current
  const now = Date.now();

  return (
    <Panel
      title="Recordings"
      description={loaded ? `${plural(recordings.length, "recording")}, newest first` : "Waiting for the server…"}
      flush
    >
      {!loaded ? (
        <EmptyState icon="film" title="Waiting for the server…" />
      ) : recordings.length === 0 ? (
        <EmptyState icon="film" title="No recordings yet">
          Start a capture from the Capture panel while a player is connected. It is saved when you stop it.
        </EmptyState>
      ) : (
        <table className={styles.table}>
          <thead>
            <tr>
              <th scope="col">Recording</th>
              <th scope="col">Length</th>
              <th scope="col">Status</th>
              <th scope="col">
                <span className="visually-hidden">Actions</span>
              </th>
            </tr>
          </thead>
          <tbody>
            {recordings.map((recording) => (
              <RecordingRow key={recording.id} recording={recording} now={now} />
            ))}
          </tbody>
        </table>
      )}
    </Panel>
  );
}
