import { api } from "../../api/client";
import type { MarkerKind } from "../../api/types";
import { Badge } from "../../components/Badge";
import { Button } from "../../components/Button";
import { formatTimestamp } from "../../lib/format";
import type { PlaybackView } from "../../state/ServerState";
import { useServerState } from "../../state/ServerState";
import { useAction } from "../../state/useAction";
import styles from "./PlaybackDeck.module.css";
import { Scrubber } from "./Scrubber";
import { usePlaybackPosition } from "./usePlaybackPosition";
import { useMarkers } from "./useRecordingDetail";

const RATES = [0.25, 0.5, 1, 2, 4];

const kindLabels: Record<MarkerKind, string> = {
  attack: "Attacks",
  weapon: "Weapon",
  furniture: "Furniture",
  area: "Area changes",
  equipment: "Equipment",
  appearance: "Appearance",
};

const statusBadge = {
  playing: { tone: "success", label: "Playing" },
  paused: { tone: "warning", label: "Paused" },
  ended: { tone: "neutral", label: "Finished" },
} as const;

export function PlaybackDeck({ playback }: { playback: PlaybackView }) {
  const { adoptPlayback } = useServerState();
  const position = usePlaybackPosition(playback);
  const markers = useMarkers(playback.recordingId);
  const [run, busy] = useAction();
  const [runSeek] = useAction();
  const [runSettings] = useAction();
  const [runClose, closing] = useAction();

  const status = statusBadge[playback.status];
  const rates = RATES.includes(playback.rate) ? RATES : [...RATES, playback.rate].sort((a, b) => a - b);

  const toggle = () =>
    run(async () => {
      const next =
        playback.status === "playing"
          ? await api.playbacks.pause(playback.id)
          : await api.playbacks.play(playback.id);
      adoptPlayback(next);
    });

  const seek = async (positionMs: number) => {
    const next = await runSeek(() => api.playbacks.seek(playback.id, positionMs));
    if (next) adoptPlayback(next);
  };

  const configure = async (settings: { rate?: number; loop?: boolean }) => {
    const next = await runSettings(() => api.playbacks.configure(playback.id, settings));
    if (next) adoptPlayback(next);
  };

  const kindCounts = Object.entries(
    markers.reduce<Record<string, number>>((counts, marker) => {
      counts[marker.kind] = (counts[marker.kind] ?? 0) + 1;
      return counts;
    }, {}),
  ) as [MarkerKind, number][];

  const toggleLabel =
    playback.status === "playing" ? "Pause" : playback.status === "ended" ? "Replay from start" : "Play";
  const toggleIcon =
    playback.status === "playing" ? "pause" : playback.status === "ended" ? "restart" : "play";

  return (
    <article className={styles.deck} aria-label={`Replay of ${playback.label}`}>
      <header className={styles.header}>
        <div className={styles.title}>
          <h3 className={styles.label}>{playback.label}</h3>
          <div className={styles.meta}>
            <span>{playback.playerName || "Unnamed player"}</span>
            <span className="mono" title="The virtual player's ID as other clients see it">
              {playback.playerId}
            </span>
          </div>
        </div>
        <Badge tone={status.tone} dot={playback.status === "playing"} pulse={playback.status === "playing"}>
          {status.label}
        </Badge>
        <Button
          variant="ghost"
          size="sm"
          icon="stop"
          busy={closing}
          onClick={() => runClose(() => api.playbacks.close(playback.id))}
          title="Stop this replay and remove its virtual player from every client"
        >
          Stop replay
        </Button>
      </header>

      {playback.error && (
        <p className={styles.error} role="alert">
          Playback failed: {playback.error}
        </p>
      )}

      <div className={styles.transport}>
        <Button
          variant="primary"
          icon={toggleIcon}
          busy={busy}
          onClick={toggle}
          aria-label={toggleLabel}
          title={toggleLabel}
          className={styles.toggle}
        />
        <div className={`${styles.time} mono tabular`} aria-hidden="true">
          {formatTimestamp(position, true)}
          <span className={styles.duration}> / {formatTimestamp(playback.durationMs, true)}</span>
        </div>
        <div className={styles.scrub}>
          <Scrubber durationMs={playback.durationMs} positionMs={position} markers={markers} onSeek={seek} />
        </div>
      </div>

      <footer className={styles.options}>
        <label className={styles.rate}>
          <span>Speed</span>
          <select
            value={playback.rate}
            onChange={(event) => void configure({ rate: Number(event.target.value) })}
          >
            {rates.map((rate) => (
              <option key={rate} value={rate}>
                {rate}×
              </option>
            ))}
          </select>
        </label>
        <Button
          variant={playback.loop ? "primary" : "secondary"}
          size="sm"
          icon="loop"
          aria-pressed={playback.loop}
          onClick={() => void configure({ loop: !playback.loop })}
        >
          Loop
        </Button>

        {kindCounts.length > 0 && (
          <ul className={styles.legend} aria-label="Timeline markers">
            {kindCounts.map(([kind, count]) => (
              <li key={kind}>
                <span className={styles.swatch} style={{ background: `var(--marker-${kind})` }} aria-hidden="true" />
                {kindLabels[kind]} {count}
              </li>
            ))}
          </ul>
        )}
      </footer>
    </article>
  );
}
