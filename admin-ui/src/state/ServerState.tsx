import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
  type ReactNode,
} from "react";
import { connectAdminFeed, type ConnectionStatus } from "../api/feed";
import type { AdminState, Ban, Batch, ChatLine, Exclusion, Player, Playback, Recording, Role, ServerInfo } from "../api/types";
import { newerPlayback } from "../lib/playbackClock";
import { awaited, predicted, type HeldPrediction, type LiveState, type PlaybackView, type Prediction } from "../lib/predictions";

export type { PlaybackView };

/** Reports how the command behind a prediction ended. */
export interface PredictionOutcome {
  succeeded: () => void;
  failed: () => void;
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
  exclusions: Exclusion[];
  bans: Ban[];
  roles: Role[];
  /**
   * Use a playback returned by a command right away instead of waiting for the next push.
   * `opened` adds one the panel does not hold yet; otherwise an unknown playback has closed and is ignored.
   */
  adoptPlayback: (playback: Playback, opened?: boolean) => void;
  /** Show a command's outcome in the lists above from now on; tell the returned outcome how the command ended. */
  predict: (prediction: Prediction) => PredictionOutcome;
}

/** A chat line numbered in arrival order, for a stable key. */
export interface ChatEntry extends ChatLine {
  seq: number;
}

const chatLimit = 200;
// How long a succeeded command's outcome is shown without the server's state showing it too.
// Pushes are at most 0.5 s apart, so only a change made elsewhere meanwhile takes this long.
const CONFIRM_TIMEOUT_MS = 5000;

interface HeldPlayback extends PlaybackView {
  /** False for a playback adopted from the command that opened it, until a snapshot lists it. */
  listed: boolean;
}

interface NumberedPrediction extends HeldPrediction {
  id: number;
}

const ServerStateContext = createContext<ServerStateValue | null>(null);
// Separate from the snapshot so a busy chat re-renders only its panel.
const ChatContext = createContext<ChatEntry[] | null>(null);

function mergePlaybacks(held: HeldPlayback[], arrived: Playback[], now: number): HeldPlayback[] {
  const heldById = new Map(held.map((playback) => [playback.id, playback]));
  const arrivedIds = new Set(arrived.map((playback) => playback.id));
  const listed = arrived.map((playback) => {
    const current = heldById.get(playback.id);
    if (current === undefined || newerPlayback(current, playback) !== current) {
      return { ...playback, receivedAt: now, listed: true };
    }
    return current.listed ? current : { ...current, listed: true };
  });
  // A snapshot produced before a playback opened can arrive after the command's response.
  return [...listed, ...held.filter((playback) => !playback.listed && !arrivedIds.has(playback.id))];
}

export function ServerStateProvider({ children }: { children: ReactNode }) {
  const [connection, setConnection] = useState<ConnectionStatus>("connecting");
  const [snapshot, setSnapshot] = useState<{ state: AdminState; receivedAt: number } | null>(null);
  const [playbacks, setPlaybacks] = useState<HeldPlayback[]>([]);
  const [predictions, setPredictions] = useState<NumberedPrediction[]>([]);
  const predictionSeq = useRef(0);
  const [chat, setChat] = useState<ChatEntry[]>([]);
  const chatSeq = useRef(0);

  useEffect(
    () =>
      connectAdminFeed({
        onStatus: setConnection,
        onChat: (line) => {
          const entry = { ...line, seq: ++chatSeq.current };
          setChat((held) => [...held.slice(Math.max(0, held.length - chatLimit + 1)), entry]);
        },
        onState: (state) => {
          const receivedAt = performance.now();
          setSnapshot({ state, receivedAt });
          setPlaybacks((held) => mergePlaybacks(held, state.playbacks, receivedAt));
        },
      }),
    [],
  );

  const adoptPlayback = useCallback((playback: Playback, opened = false) => {
    setPlaybacks((held) => {
      const current = held.find((existing) => existing.id === playback.id);
      if (current === undefined) {
        return opened ? [...held, { ...playback, receivedAt: performance.now(), listed: false }] : held;
      }
      if (newerPlayback(current, playback) === current) return held;
      const adopted = { ...playback, receivedAt: performance.now(), listed: current.listed };
      // Replace in place so decks keep their order.
      return held.map((existing) => (existing.id === playback.id ? adopted : existing));
    });
    if (opened) {
      // Never listed: it closed again before a snapshot was produced.
      window.setTimeout(
        () => setPlaybacks((held) => held.filter((existing) => existing.listed || existing.id !== playback.id)),
        CONFIRM_TIMEOUT_MS,
      );
    }
  }, []);

  const predict = useCallback((prediction: Prediction): PredictionOutcome => {
    const id = ++predictionSeq.current;
    const drop = () => setPredictions((held) => held.filter((entry) => entry.id !== id));
    setPredictions((held) => [
      ...held.filter((entry) => prediction.key === undefined || entry.prediction.key !== prediction.key),
      { id, prediction, succeeded: false },
    ]);
    return {
      succeeded: () => {
        setPredictions((held) => held.map((entry) => (entry.id === id ? { ...entry, succeeded: true } : entry)));
        window.setTimeout(drop, CONFIRM_TIMEOUT_MS);
      },
      failed: drop,
    };
  }, []);

  const truth = useMemo<LiveState>(
    () => ({
      receivedAt: snapshot?.receivedAt ?? 0,
      players: snapshot?.state.players ?? [],
      recordings: snapshot?.state.recordings ?? [],
      playbacks,
      batches: snapshot?.state.batches ?? [],
      roles: snapshot?.state.roles ?? [],
    }),
    [snapshot, playbacks],
  );

  useEffect(() => {
    if (predictions.some((entry) => !awaited(entry, truth))) {
      setPredictions((held) => held.filter((entry) => awaited(entry, truth)));
    }
  }, [predictions, truth]);

  const value = useMemo<ServerStateValue>(() => {
    const shown = predicted(truth, predictions);
    return {
      connection,
      loaded: snapshot !== null,
      server: snapshot?.state.server ?? null,
      snapshotReceivedAt: snapshot?.receivedAt ?? null,
      players: shown.players,
      recordings: shown.recordings,
      playbacks: shown.playbacks,
      batches: shown.batches,
      exclusions: snapshot?.state.exclusions ?? [],
      bans: snapshot?.state.bans ?? [],
      roles: shown.roles,
      adoptPlayback,
      predict,
    };
  }, [connection, snapshot, truth, predictions, adoptPlayback, predict]);

  return (
    <ServerStateContext.Provider value={value}>
      <ChatContext.Provider value={chat}>{children}</ChatContext.Provider>
    </ServerStateContext.Provider>
  );
}

export function useServerState(): ServerStateValue {
  const value = useContext(ServerStateContext);
  if (value === null) throw new Error("useServerState must be used inside ServerStateProvider");
  return value;
}

/** Global chat lines received since the page opened, oldest first, at most the last 200. */
export function useChat(): ChatEntry[] {
  const value = useContext(ChatContext);
  if (value === null) throw new Error("useChat must be used inside ServerStateProvider");
  return value;
}
