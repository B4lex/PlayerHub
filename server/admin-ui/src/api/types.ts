// Wire types of the server's admin API (server/src/server/admin/schemas.py).

export type RecordingStatus = "recording" | "complete" | "interrupted";
export type PlaybackStatus = "playing" | "paused" | "ended";
export type BatchStatus = "starting" | "running" | "paused";
export type BotStatus = "pending" | "playing" | "paused" | "failed";
/** `virtual`: replayed inside the server. `socket`: a loopback Socket.IO client per bot. */
export type BatchTransport = "virtual" | "socket";
export type MarkerKind = "attack" | "appearance" | "equipment" | "weapon" | "furniture" | "area";

export interface Position {
  x: number;
  y: number;
  z: number;
}

/** A connected player, or the virtual player of a running replay. */
export interface Player {
  id: string;
  name: string;
  virtual: boolean;
  /** Placed in the game world, not in the main menu or a loading screen. */
  inWorld: boolean;
  connectedAtMs: number | null;
  cellId: number | null;
  worldspaceId: number | null;
  position: Position | null;
  movementState: string | null;
  /** The recording being captured from this player, if any. */
  recordingId: string | null;
  /** The replay this virtual player belongs to. */
  playbackId: string | null;
}

export interface Recording {
  id: string;
  label: string;
  playerId: string;
  playerName: string;
  /** ISO-8601, UTC. */
  startedAt: string;
  durationMs: number;
  messageCount: number;
  sizeBytes: number;
  status: RecordingStatus;
  endReason: string | null;
}

export interface Playback {
  id: string;
  recordingId: string;
  label: string;
  /** Id of the virtual player other clients see. */
  playerId: string;
  playerName: string;
  status: PlaybackStatus;
  /** Timeline position at the moment the server produced this state. */
  positionMs: number;
  durationMs: number;
  rate: number;
  loop: boolean;
  /** Rises on every change; a higher revision supersedes a lower one. */
  revision: number;
  error: string | null;
}

export interface BatchSettings {
  players: number;
  transport: BatchTransport;
  seed: number;
  /** Share of the spacing between bots that each may be moved by, 0 to 1. */
  jitter: number;
  /** Each bot's speed varies within 1 ± this. */
  rateSpread: number;
  rampUpSeconds: number;
}

export interface Bot {
  index: number;
  name: string;
  /** Assigned once the bot joins. */
  playerId: string | null;
  status: BotStatus;
  startMs: number;
  rate: number;
  /** Timeline position when the server produced this state. */
  positionMs: number;
  error: string | null;
}

/** A load-test batch: one recording replayed by many looping bots. */
export interface Batch {
  id: string;
  recordingId: string;
  label: string;
  playerName: string;
  /** ISO-8601, UTC. */
  createdAt: string;
  durationMs: number;
  status: BatchStatus;
  settings: BatchSettings;
  bots: Bot[];
}

export interface ServerInfo {
  startedAtMs: number;
  serverTimeMs: number;
}

/** Everything the panel shows, replicated as one snapshot. */
export interface AdminState {
  server: ServerInfo;
  players: Player[];
  recordings: Recording[];
  playbacks: Playback[];
  batches: Batch[];
}

export interface Marker {
  tMs: number;
  kind: MarkerKind;
  label: string;
}

export interface RecordingDetail extends Recording {
  markers: Marker[];
}

export interface OpenPlaybackRequest {
  recordingId: string;
  autoplay?: boolean;
  rate?: number;
  loop?: boolean;
  startMs?: number;
}

export interface PlaybackSettings {
  rate?: number;
  loop?: boolean;
}

export interface CreateBatchRequest {
  recordingId: string;
  players: number;
  transport?: BatchTransport;
  /** Omit for a random one; the batch reports the seed it used. */
  seed?: number;
  jitter?: number;
  rateSpread?: number;
  rampUpSeconds?: number;
}
