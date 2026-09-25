import { useState } from "react";
import { api } from "../../api/client";
import type { Batch, Bot, BotStatus } from "../../api/types";
import { Badge } from "../../components/Badge";
import { Button } from "../../components/Button";
import { ConfirmButton } from "../../components/ConfirmButton";
import { botPositionAt } from "../../lib/batchClock";
import { formatDateTime, formatTimestamp, plural, shortId } from "../../lib/format";
import { useAction } from "../../state/useAction";
import styles from "./BatchCard.module.css";

const transportLabels = {
  virtual: { label: "In-server", title: "Bots are replayed inside the server and relayed to every client" },
  socket: { label: "Socket clients", title: "Each bot is a loopback Socket.IO client that goes through the player ingestion path" },
} as const;

const botStatusLabels: Record<BotStatus, string> = {
  pending: "Waiting to join",
  playing: "Playing",
  paused: "Paused",
  failed: "Failed",
};

function percent(value: number): string {
  return `${Math.round(value * 100)}%`;
}

function StatusBadge({ batch, joined }: { batch: Batch; joined: number }) {
  switch (batch.status) {
    case "starting":
      return (
        <Badge tone="info" dot pulse>
          Joining {joined}/{batch.bots.length}
        </Badge>
      );
    case "running":
      return (
        <Badge tone="success" dot pulse>
          Running
        </Badge>
      );
    case "paused":
      return <Badge tone="warning">Paused</Badge>;
  }
}

/** Every bot as a tick at its position in the shared recording. */
function Timeline({ batch, elapsedMs }: { batch: Batch; elapsedMs: number }) {
  const duration = Math.max(batch.durationMs, 1);
  return (
    <div className={styles.timeline}>
      <div className={styles.track} role="img" aria-label="Where each bot is in the recording">
        {batch.bots
          .filter((bot) => bot.status === "playing" || bot.status === "paused")
          .map((bot) => (
            <span
              key={bot.index}
              className={styles.tick}
              style={{ left: `${(botPositionAt(bot, batch.durationMs, elapsedMs) / duration) * 100}%` }}
            />
          ))}
      </div>
      <div className={`${styles.scale} mono tabular`} aria-hidden="true">
        <span>0:00</span>
        <span>{formatTimestamp(batch.durationMs)}</span>
      </div>
    </div>
  );
}

function BotRow({ bot, batch, elapsedMs }: { bot: Bot; batch: Batch; elapsedMs: number }) {
  const joined = bot.status === "playing" || bot.status === "paused";
  return (
    <tr>
      <td className="tabular">{bot.index + 1}</td>
      <td>
        <div className={styles.botName}>{bot.name}</div>
        {bot.playerId !== null && (
          <div className={`${styles.sub} mono`} title={bot.playerId}>
            {shortId(bot.playerId)}
          </div>
        )}
      </td>
      <td className="mono tabular">{joined ? formatTimestamp(botPositionAt(bot, batch.durationMs, elapsedMs)) : "—"}</td>
      <td className="mono tabular">{bot.rate}×</td>
      <td className={bot.status === "failed" ? styles.failed : styles.sub}>
        {bot.error !== null ? `${botStatusLabels.failed}: ${bot.error}` : botStatusLabels[bot.status]}
      </td>
    </tr>
  );
}

export function BatchCard({ batch, elapsedMs }: { batch: Batch; elapsedMs: number }) {
  const [run, busy] = useAction();
  const [runClose, closing] = useAction();
  const [showBots, setShowBots] = useState(false);
  const { settings } = batch;
  const joined = batch.bots.filter((bot) => bot.playerId !== null).length;
  const failed = batch.bots.filter((bot) => bot.status === "failed");
  const transport = transportLabels[settings.transport];
  const spacingMs = batch.durationMs / settings.players;
  const paused = batch.status === "paused";

  const toggle = () => run(() => (paused ? api.batches.resume(batch.id) : api.batches.pause(batch.id)));

  return (
    <article className={styles.card} aria-label={`Batch replaying ${batch.label}`}>
      <header className={styles.header}>
        <div className={styles.title}>
          <h3 className={styles.label}>{batch.label}</h3>
          <div className={styles.meta}>
            <span>{plural(settings.players, "bot")}</span>
            <span title={transport.title}>{transport.label}</span>
            <span>
              seed <span className="mono">{settings.seed}</span>
            </span>
            <time dateTime={batch.createdAt} title={`Started ${formatDateTime(batch.createdAt)}`}>
              {new Date(batch.createdAt).toLocaleTimeString()}
            </time>
          </div>
        </div>
        <StatusBadge batch={batch} joined={joined} />
        <div className={styles.controls}>
          <Button size="sm" icon={paused ? "play" : "pause"} busy={busy} onClick={toggle}>
            {paused ? "Resume" : "Pause"}
          </Button>
          <ConfirmButton
            variant="ghost"
            size="sm"
            icon="stop"
            busy={closing}
            confirmLabel="Remove all bots?"
            title="Stop this batch and remove its bots from every client"
            onConfirm={() => runClose(() => api.batches.close(batch.id))}
          >
            Stop
          </ConfirmButton>
        </div>
      </header>

      {failed.length > 0 && (
        <p className={styles.error} role="alert">
          {plural(failed.length, "bot")} failed: {failed[0]?.error}
          {failed.length > 1 && " (and others; see the bot list)"}
        </p>
      )}

      <Timeline batch={batch} elapsedMs={elapsedMs} />

      <dl className={styles.facts}>
        <div>
          <dt>Spacing</dt>
          <dd className="tabular">{(spacingMs / 1000).toFixed(1)} s</dd>
        </div>
        <div>
          <dt>Jitter</dt>
          <dd className="tabular">{percent(settings.jitter)}</dd>
        </div>
        <div>
          <dt>Speed</dt>
          <dd className="tabular">{settings.rateSpread > 0 ? `1× ± ${percent(settings.rateSpread)}` : "1×"}</dd>
        </div>
        <div>
          <dt>Ramp-up</dt>
          <dd className="tabular">{settings.rampUpSeconds > 0 ? `${settings.rampUpSeconds} s` : "None"}</dd>
        </div>
      </dl>

      <div>
        <Button variant="ghost" size="sm" aria-expanded={showBots} onClick={() => setShowBots((shown) => !shown)}>
          {showBots ? "Hide bots" : `Show ${plural(batch.bots.length, "bot")}`}
        </Button>
        {showBots && (
          <div className={styles.bots}>
            <table className={styles.table}>
              <thead>
                <tr>
                  <th scope="col">#</th>
                  <th scope="col">Bot</th>
                  <th scope="col">Position</th>
                  <th scope="col">Speed</th>
                  <th scope="col">Status</th>
                </tr>
              </thead>
              <tbody>
                {batch.bots.map((bot) => (
                  <BotRow key={bot.index} bot={bot} batch={batch} elapsedMs={elapsedMs} />
                ))}
              </tbody>
            </table>
          </div>
        )}
      </div>
    </article>
  );
}
