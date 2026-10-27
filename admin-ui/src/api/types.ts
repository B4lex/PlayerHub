// Wire types of the server's admin API (server/internal/admin).

export type RecordingStatus = "recording" | "complete" | "interrupted";
export type PlaybackStatus = "playing" | "paused" | "ended";
export type BatchStatus = "starting" | "running" | "paused";
export type BotStatus = "pending" | "playing" | "paused" | "failed";
export type MarkerKind = "attack" | "appearance" | "equipment" | "weapon" | "furniture" | "area";

/** A form named by its plugin and its ID within that plugin. */
export interface FormRef {
  plugin: string;
  formId: number;
}

export interface Position {
  x: number;
  y: number;
  z: number;
}

/** A connected player, or the virtual player of a running replay. */
export interface Player {
  id: string;
  name: string;
  /** The address a connected player joined from, after trusted proxies; null for virtual players. */
  address: string | null;
  virtual: boolean;
  /** Placed in the game world, not in the main menu or a loading screen. */
  inWorld: boolean;
  connectedAtMs: number | null;
  /** One of these names the player's space; both are null before its first context. */
  interiorCell: FormRef | null;
  worldspace: FormRef | null;
  position: Position | null;
  movementState: string | null;
  /** Peers this player receives. */
  visible: number;
  /** Hidden from every other player by an admin. */
  hidden: boolean;
  /** Bytes per second the server writes to this client. */
  bytesPerSecond: number;
  /** Checkpoints per second replaced by a newer one before they were written: a slow link. */
  replacedPerSecond: number;
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
  /** Bots are always replayed inside the server; cmd/loadgen runs networked ones. */
  transport: "virtual";
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

/** A directed pair: the viewer never receives the target. */
export interface Exclusion {
  viewerId: string;
  targetId: string;
}

/** Refuses a session, and the address it was banned from if one is given, until lifted. */
export interface Ban {
  id: string;
  sessionId: string;
  /** The session's display name when it was banned. */
  displayName: string;
  /** An IPv4 address or an IPv6 /64; null when only the session is banned. */
  address: string | null;
  createdAtMs: number;
}

/** A player's identity on this server, live or banned. */
export interface Session {
  id: string;
  displayName: string;
  createdAtMs: number;
  lastUsedAtMs: number;
  /** Null for a banned session, which never expires. */
  expiresAtMs: number | null;
  banned: boolean;
  /** The slot of the player connected with this session. */
  playerId: string | null;
}

export interface SessionPage {
  sessions: Session[];
  /** Every session matching the search, not just this page. */
  total: number;
}

/** Everything the panel shows, replicated as one snapshot. */
export interface AdminState {
  server: ServerInfo;
  players: Player[];
  recordings: Recording[];
  playbacks: Playback[];
  batches: Batch[];
  exclusions: Exclusion[];
  bans: Ban[];
}

/** A Global chat line, pushed once as it is relayed; the server keeps none. */
export interface ChatLine {
  atMs: number;
  /** The speaker's slot. */
  playerId: string;
  name: string;
  text: string;
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
  /** Omit for a random one; the batch reports the seed it used. */
  seed?: number;
  jitter?: number;
  rateSpread?: number;
  rampUpSeconds?: number;
}
