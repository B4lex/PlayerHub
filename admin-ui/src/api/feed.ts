import type { AdminState, ChatLine } from "./types";

export type ConnectionStatus = "connecting" | "connected" | "disconnected";

export interface AdminFeedHandlers {
  onState: (state: AdminState) => void;
  onChat: (line: ChatLine) => void;
  onStatus: (status: ConnectionStatus) => void;
}

/**
 * Subscribe to the server's live state, a server-sent event stream. The server
 * pushes a complete snapshot on connect and after every change, and each Global
 * chat line once as it is relayed, so lines sent while disconnected are missed;
 * commands go through the REST client instead. EventSource reconnects by itself.
 * Returns a function that closes the stream.
 */
export function connectAdminFeed({ onState, onChat, onStatus }: AdminFeedHandlers): () => void {
  const source = new EventSource("/api/events");

  source.addEventListener("open", () => onStatus("connected"));
  source.addEventListener("error", () =>
    onStatus(source.readyState === EventSource.CLOSED ? "disconnected" : "connecting"),
  );
  source.addEventListener("state", (event) => onState(JSON.parse((event as MessageEvent<string>).data) as AdminState));
  source.addEventListener("chat", (event) => onChat(JSON.parse((event as MessageEvent<string>).data) as ChatLine));

  onStatus("connecting");
  return () => source.close();
}
