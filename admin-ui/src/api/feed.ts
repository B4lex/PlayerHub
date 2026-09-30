import type { AdminState } from "./types";

export type ConnectionStatus = "connecting" | "connected" | "disconnected";

export interface AdminFeedHandlers {
  onState: (state: AdminState) => void;
  onStatus: (status: ConnectionStatus) => void;
}

/**
 * Subscribe to the server's live state, a server-sent event stream. The server
 * pushes a complete snapshot on connect and after every change; commands go
 * through the REST client instead. EventSource reconnects by itself. Returns a
 * function that closes the stream.
 */
export function connectAdminFeed({ onState, onStatus }: AdminFeedHandlers): () => void {
  const source = new EventSource("/api/events");

  source.addEventListener("open", () => onStatus("connected"));
  source.addEventListener("error", () =>
    onStatus(source.readyState === EventSource.CLOSED ? "disconnected" : "connecting"),
  );
  source.addEventListener("state", (event) => onState(JSON.parse((event as MessageEvent<string>).data) as AdminState));

  onStatus("connecting");
  return () => source.close();
}
