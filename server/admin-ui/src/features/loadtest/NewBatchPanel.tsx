import { useState, type FormEvent } from "react";
import { api } from "../../api/client";
import type { BatchTransport } from "../../api/types";
import { Button } from "../../components/Button";
import { EmptyState } from "../../components/EmptyState";
import { Panel } from "../../components/Panel";
import { useToast } from "../../components/Toasts";
import { formatTimestamp, plural } from "../../lib/format";
import { useServerState } from "../../state/ServerState";
import { useAction } from "../../state/useAction";
import styles from "./NewBatchPanel.module.css";

// Mirrors the server's limits (server/src/server/loadtest/plan.py).
const MAXIMUM_BOTS = 500;
const MAXIMUM_RAMP_UP_SECONDS = 600;
const MAXIMUM_RATE_SPREAD_PERCENT = 50;
const MAXIMUM_SEED = 2 ** 31 - 1;

const transports: { value: BatchTransport; label: string; description: string }[] = [
  {
    value: "virtual",
    label: "In-server",
    description: "Replayed inside the server. Lightest; loads the game clients and the server's fan-out.",
  },
  {
    value: "socket",
    label: "Socket clients",
    description: "One loopback Socket.IO connection per bot, through the same ingestion path as real players.",
  },
];

function parseInteger(text: string, minimum: number, maximum: number): number | null {
  if (!/^\d+$/.test(text.trim())) return null;
  const value = Number(text);
  return value >= minimum && value <= maximum ? value : null;
}

export function NewBatchPanel() {
  const { loaded, recordings } = useServerState();
  const toast = useToast();
  const [run, busy] = useAction();
  const playable = recordings.filter((recording) => recording.status !== "recording");

  const [chosenId, setRecordingId] = useState("");
  const [players, setPlayers] = useState("10");
  const [transport, setTransport] = useState<BatchTransport>("virtual");
  const [rampUp, setRampUp] = useState("0");
  const [jitterPercent, setJitterPercent] = useState(50);
  const [spreadPercent, setSpreadPercent] = useState(0);
  const [seed, setSeed] = useState("");

  // Until one is chosen, or if the chosen one is deleted, the newest recording.
  const recording = playable.find((candidate) => candidate.id === chosenId) ?? playable[0];
  const playerCount = parseInteger(players, 1, MAXIMUM_BOTS);
  const rampUpSeconds = parseInteger(rampUp, 0, MAXIMUM_RAMP_UP_SECONDS);
  const seedValue = seed.trim() === "" ? undefined : parseInteger(seed, 0, MAXIMUM_SEED);
  const valid = recording !== undefined && playerCount !== null && rampUpSeconds !== null && seedValue !== null;

  const submit = async (event: FormEvent) => {
    event.preventDefault();
    if (!valid) return;
    const batch = await run(() =>
      api.batches.create({
        recordingId: recording.id,
        players: playerCount,
        transport,
        seed: seedValue,
        jitter: jitterPercent / 100,
        rateSpread: spreadPercent / 100,
        rampUpSeconds,
      }),
    );
    if (batch) toast.success(`Started ${plural(batch.settings.players, "bot")} with seed ${batch.settings.seed}`);
  };

  if (loaded && playable.length === 0) {
    return (
      <Panel title="New batch">
        <EmptyState icon="film" title="No recordings yet">
          Capture a player on the Replay management page first; a batch replays one recording.
        </EmptyState>
      </Panel>
    );
  }

  return (
    <Panel title="New batch" description="Bots are spread evenly over the recording and loop it.">
      <form className={styles.form} onSubmit={submit}>
        <label className={styles.field}>
          <span>Recording</span>
          <select value={recording?.id ?? ""} onChange={(event) => setRecordingId(event.target.value)} disabled={!loaded}>
            {playable.map((candidate) => (
              <option key={candidate.id} value={candidate.id}>
                {candidate.label} ({formatTimestamp(candidate.durationMs)})
              </option>
            ))}
          </select>
        </label>

        <div className={styles.pair}>
          <label className={styles.field}>
            <span>Bots</span>
            <input
              type="number"
              inputMode="numeric"
              min={1}
              max={MAXIMUM_BOTS}
              value={players}
              aria-invalid={playerCount === null}
              onChange={(event) => setPlayers(event.target.value)}
            />
          </label>
          <label className={styles.field}>
            <span>Ramp-up (s)</span>
            <input
              type="number"
              inputMode="numeric"
              min={0}
              max={MAXIMUM_RAMP_UP_SECONDS}
              value={rampUp}
              aria-invalid={rampUpSeconds === null}
              title="Bots join one by one over this time instead of all at once"
              onChange={(event) => setRampUp(event.target.value)}
            />
          </label>
        </div>

        <fieldset className={styles.transports}>
          <legend>Connection</legend>
          {transports.map((option) => (
            <label key={option.value} className={styles.transport}>
              <input
                type="radio"
                name="transport"
                value={option.value}
                checked={transport === option.value}
                onChange={() => setTransport(option.value)}
              />
              <span>
                <span className={styles.transportLabel}>{option.label}</span>
                <span className={styles.hint}>{option.description}</span>
              </span>
            </label>
          ))}
        </fieldset>

        <label className={styles.field}>
          <span>
            Jitter <span className="tabular">{jitterPercent}%</span>
          </span>
          <input
            type="range"
            min={0}
            max={100}
            step={5}
            value={jitterPercent}
            onChange={(event) => setJitterPercent(Number(event.target.value))}
          />
          <span className={styles.hint}>How far each bot may move from its even slot, as a share of the spacing.</span>
        </label>

        <label className={styles.field}>
          <span>
            Speed variation <span className="tabular">±{spreadPercent}%</span>
          </span>
          <input
            type="range"
            min={0}
            max={MAXIMUM_RATE_SPREAD_PERCENT}
            step={1}
            value={spreadPercent}
            onChange={(event) => setSpreadPercent(Number(event.target.value))}
          />
          <span className={styles.hint}>Bots drift apart over time instead of repeating the same spacing every loop.</span>
        </label>

        <label className={styles.field}>
          <span>Seed</span>
          <input
            type="text"
            inputMode="numeric"
            placeholder="Random"
            value={seed}
            aria-invalid={seedValue === null}
            onChange={(event) => setSeed(event.target.value)}
          />
          <span className={styles.hint}>Reuse a batch's seed to get the same layout when comparing builds.</span>
        </label>

        {recording !== undefined && playerCount !== null && (
          <p className={styles.summary}>
            One bot every <strong className="tabular">{(recording.durationMs / playerCount / 1000).toFixed(1)} s</strong>{" "}
            of the {formatTimestamp(recording.durationMs)} recording
            {rampUpSeconds ? `, joining over ${rampUpSeconds} s` : ""}.
          </p>
        )}

        <Button type="submit" variant="primary" icon="play" busy={busy} disabled={!valid}>
          Start batch
        </Button>
      </form>
    </Panel>
  );
}
