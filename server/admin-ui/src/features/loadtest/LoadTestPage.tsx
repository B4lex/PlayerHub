import { EmptyState } from "../../components/EmptyState";
import { PageHeader } from "../../components/PageHeader";
import { Panel } from "../../components/Panel";
import { plural } from "../../lib/format";
import { useTicker } from "../../lib/useTicker";
import { useServerState } from "../../state/ServerState";
import { BatchCard } from "./BatchCard";
import styles from "./LoadTestPage.module.css";
import { NewBatchPanel } from "./NewBatchPanel";

export function LoadTestPage() {
  const { loaded, batches, snapshotReceivedAt } = useServerState();
  const moving = batches.some((batch) => batch.status !== "paused");
  const now = useTicker(250, moving);
  const elapsedMs = snapshotReceivedAt === null ? 0 : now - snapshotReceivedAt;
  const bots = batches.reduce((total, batch) => total + batch.bots.filter((bot) => bot.playerId !== null).length, 0);

  return (
    <>
      <PageHeader
        title="Load testing"
        description="Replay a recording with many bots at once, spread over its timeline so each is doing something different. Batches run until stopped or the server restarts."
      />

      <div className={styles.columns}>
        <Panel
          title="Batches"
          description={
            loaded
              ? batches.length === 0
                ? "Nothing running"
                : `${plural(batches.length, "batch", "batches")}, ${plural(bots, "bot")} in the world`
              : "Waiting for the server…"
          }
        >
          {batches.length === 0 ? (
            <EmptyState icon="gauge" title={loaded ? "No batches running" : "Waiting for the server…"}>
              {loaded && "Choose a recording and how many bots should replay it, then start a batch."}
            </EmptyState>
          ) : (
            <div className={styles.batches}>
              {batches.map((batch) => (
                <BatchCard key={batch.id} batch={batch} elapsedMs={elapsedMs} />
              ))}
            </div>
          )}
        </Panel>

        <NewBatchPanel />
      </div>
    </>
  );
}
