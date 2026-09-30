import { EmptyState } from "../../components/EmptyState";
import { PageHeader } from "../../components/PageHeader";
import { Panel } from "../../components/Panel";
import { useServerState } from "../../state/ServerState";
import { CapturePanel } from "./CapturePanel";
import { PlaybackDeck } from "./PlaybackDeck";
import styles from "./ReplaysPage.module.css";
import { RecordingsPanel } from "./RecordingsPanel";

export function ReplaysPage() {
  const { loaded, playbacks } = useServerState();

  return (
    <>
      <PageHeader
        title="Replay management"
        description="Capture what players do, then replay it as a virtual player that everyone connected can see."
      />

      <div className={styles.stack}>
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
