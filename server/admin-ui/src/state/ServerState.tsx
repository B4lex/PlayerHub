import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useState,
  type ReactNode,
} from "react";
import { connectAdminFeed, type ConnectionStatus } from "../api/feed";
import type { AdminState, Batch, Player, Playback, Recording, ServerInfo } from "../api/types";
import { newerPlayback } from "../lib/playbackClock";

/** A playback plus the local time its state arrived, so its position can be extrapolated. */
export interface PlaybackView extends Playback {
  receivedAt: number;
}

interface ServerStateValue {
  connection: ConnectionStatus;
  /** False until the first snapshot arrives. */
  loaded: boolean;
  server: ServerInfo | null;
  /** Local `performance.now()` when the current snapshot arrived; pairs with `server.serverTimeMs`. */
  snapshotReceivedAt: number | null;
  players: Player[];
  recordings: Recording[];
  playbacks: PlaybackView[];
  /** Load-test batches. Their bots are not in `players`. */
  batches: Batch[];
  /** Use a playback returned by a command right away instead of waiting for the next push. */
  adoptPlayback: (playback: Playback) => void;
}

const ServerStateContext = createContext<ServerStateValue | null>(null);

function mergePlaybacks(held: PlaybackView[], arrived: Playback[], now: number): PlaybackView[] {
  const heldById = new Map(held.map((playback) => [playback.id, playback]));
  return arrived.map((playback) => {
    const current = heldById.get(playback.id);
    const chosen = newerPlayback(current, playback);
    return chosen === current && current !== undefined ? current : { ...playback, receivedAt: now };
  });
}

export function ServerStateProvider({ children }: { children: ReactNode }) {
  const [connection, setConnection] = useState<ConnectionStatus>("connecting");
  const [snapshot, setSnapshot] = useState<{ state: AdminState; receivedAt: number } | null>(null);
  const [playbacks, setPlaybacks] = useState<PlaybackView[]>([]);

  useEffect(
    () =>
      connectAdminFeed({
        onStatus: setConnection,
        onState: (state) => {
          const receivedAt = performance.now();
          setSnapshot({ state, receivedAt });
          setPlaybacks((held) => mergePlaybacks(held, state.playbacks, receivedAt));
        },
      }),
    [],
  );

  const adoptPlayback = useCallback((playback: Playback) => {
    setPlaybacks((held) => {
      const current = held.find((existing) => existing.id === playback.id);
      if (current !== undefined && newerPlayback(current, playback) === current) return held;
      const adopted = { ...playback, receivedAt: performance.now() };
      // Replace in place so decks keep their order.
      return current === undefined
        ? [...held, adopted]
        : held.map((existing) => (existing.id === playback.id ? adopted : existing));
    });
  }, []);

  const value = useMemo<ServerStateValue>(
    () => ({
      connection,
      loaded: snapshot !== null,
      server: snapshot?.state.server ?? null,
      snapshotReceivedAt: snapshot?.receivedAt ?? null,
      players: snapshot?.state.players ?? [],
      recordings: snapshot?.state.recordings ?? [],
      playbacks,
      batches: snapshot?.state.batches ?? [],
      adoptPlayback,
    }),
    [connection, snapshot, playbacks, adoptPlayback],
  );

  return <ServerStateContext.Provider value={value}>{children}</ServerStateContext.Provider>;
}

export function useServerState(): ServerStateValue {
  const value = useContext(ServerStateContext);
  if (value === null) throw new Error("useServerState must be used inside ServerStateProvider");
  return value;
}
