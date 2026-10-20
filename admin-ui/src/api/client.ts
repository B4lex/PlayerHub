import type {
  Ban,
  Batch,
  CreateBatchRequest,
  Exclusion,
  OpenPlaybackRequest,
  Playback,
  PlaybackSettings,
  Recording,
  RecordingDetail,
} from "./types";

/** A failed API call, carrying the server's explanation. */
export class ApiError extends Error {
  readonly status: number;

  constructor(status: number, message: string) {
    super(message);
    this.name = "ApiError";
    this.status = status;
  }
}

async function describeFailure(response: Response): Promise<string> {
  try {
    const body: unknown = await response.json();
    if (typeof body === "object" && body !== null && "detail" in body) {
      const { detail } = body;
      if (typeof detail === "string") return detail;
      // Request validation errors are a list of { msg, loc }.
      if (Array.isArray(detail)) {
        return detail
          .map((item: { msg?: string }) => item.msg ?? "invalid request")
          .join("; ");
      }
    }
  } catch {
    // Not JSON; fall through to the status text.
  }
  return response.statusText || `Request failed (${response.status})`;
}

async function request<T>(method: string, path: string, body?: unknown): Promise<T> {
  let response: Response;
  try {
    response = await fetch(path, {
      method,
      headers: body === undefined ? undefined : { "Content-Type": "application/json" },
      body: body === undefined ? undefined : JSON.stringify(body),
    });
  } catch {
    throw new ApiError(0, "Cannot reach the server");
  }
  if (!response.ok) throw new ApiError(response.status, await describeFailure(response));
  if (response.status === 204) return undefined as T;
  return (await response.json()) as T;
}

const enc = encodeURIComponent;

export const api = {
  players: {
    /** Hide a player from everyone else, or show it again. */
    setHidden: (id: string, hidden: boolean) =>
      request<{ hidden: boolean }>("PUT", `/api/players/${enc(id)}/hidden`, { hidden }),
    /** Disconnect a player, whose game then waits for them to connect again. */
    kick: (id: string) => request<void>("POST", `/api/players/${enc(id)}/kick`),
    /** Disconnect a player and refuse their session, and with `address` everyone from their address. */
    ban: (id: string, address: boolean) => request<Ban>("POST", `/api/players/${enc(id)}/ban`, { address }),
  },
  bans: {
    lift: (id: string) => request<void>("DELETE", `/api/bans/${enc(id)}`),
  },
  exclusions: {
    /** The viewer stops receiving the target. */
    add: (exclusion: Exclusion) => request<Exclusion>("POST", "/api/exclusions", exclusion),
    remove: (exclusion: Exclusion) => request<Exclusion>("DELETE", "/api/exclusions", exclusion),
  },
  recordings: {
    get: (id: string) => request<RecordingDetail>("GET", `/api/recordings/${enc(id)}`),
    /** Start capturing a connected player until stopped. */
    start: (playerId: string, label = "") =>
      request<Recording>("POST", "/api/recordings", { playerId, label }),
    stop: (id: string) => request<Recording>("POST", `/api/recordings/${enc(id)}/stop`),
    remove: (id: string) => request<void>("DELETE", `/api/recordings/${enc(id)}`),
  },
  playbacks: {
    /** Replay a recording as a new virtual player. */
    open: (options: OpenPlaybackRequest) => request<Playback>("POST", "/api/playbacks", options),
    play: (id: string) => request<Playback>("POST", `/api/playbacks/${enc(id)}/play`),
    pause: (id: string) => request<Playback>("POST", `/api/playbacks/${enc(id)}/pause`),
    seek: (id: string, positionMs: number) =>
      request<Playback>("POST", `/api/playbacks/${enc(id)}/seek`, { positionMs }),
    configure: (id: string, settings: PlaybackSettings) =>
      request<Playback>("PATCH", `/api/playbacks/${enc(id)}`, settings),
    close: (id: string) => request<void>("DELETE", `/api/playbacks/${enc(id)}`),
  },
  batches: {
    /** Replay a recording with many bots spread over its timeline. */
    create: (options: CreateBatchRequest) => request<Batch>("POST", "/api/batches", options),
    pause: (id: string) => request<Batch>("POST", `/api/batches/${enc(id)}/pause`),
    resume: (id: string) => request<Batch>("POST", `/api/batches/${enc(id)}/resume`),
    close: (id: string) => request<void>("DELETE", `/api/batches/${enc(id)}`),
  },
};
