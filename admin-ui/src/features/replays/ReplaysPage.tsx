import { EmptyState } from "../../components/EmptyState";
import { PageHeader } from "../../components/PageHeader";
import { Panel } from "../../components/Panel";
import { StatTile } from "../../components/StatTile";
import { formatBytes } from "../../lib/format";
import { useServerState } from "../../state/ServerState";
import { CapturePanel } from "./CapturePanel";
import { PlaybackDeck } from "./PlaybackDeck";
import styles from "./ReplaysPage.module.css";
import { RecordingsPanel } from "./RecordingsPanel";

export function ReplaysPage() {
  const { loaded, recordings, playbacks } = useServerState();

  const capturing = recordings.filter((recording) => recording.status === "recording");
  const stored = recordings.filter((recording) => recording.status !== "recording");
  const playing = playbacks.filter((playback) => playback.status === "playing");
  const storedBytes = stored.reduce((total, recording) => total + recording.sizeBytes, 0);

  return (
    <>
      <PageHeader
        title="Replay management"
        description="Capture what players do, then replay it as a virtual player that everyone connected can see."
      />

      <div className={styles.stack}>
        <div className={styles.tiles}>
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
        </div>

        <Panel
          title="Now playing"
          description="Each replay is a virtual player. Seek, pause, or change its speed, and every client follows."
        >
          {playbacks.length === 0 ? (
            <EmptyState icon="replay" title={loaded ? "Nothing is playing" : "Waiting for the server…"}>
              {loaded && "Press Play on a recording below to spawn it in the world."}
            </EmptyState>
          ) : (
            <div className={styles.decks}>
              {playbacks.map((playback) => (
                <PlaybackDeck key={playback.id} playback={playback} />
              ))}
            </div>
          )}
        </Panel>

        <div className={styles.columns}>
          <RecordingsPanel />
          <CapturePanel />
        </div>
      </div>
    </>
  );
}
