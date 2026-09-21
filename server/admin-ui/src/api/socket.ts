import { io, type Socket } from "socket.io-client";
import type { AdminState } from "./types";

export type ConnectionStatus = "connecting" | "connected" | "disconnected";

export interface AdminSocketHandlers {
  onState: (state: AdminState) => void;
  onStatus: (status: ConnectionStatus) => void;
}

/**
 * Subscribe to the server's live state over the /admin namespace. The server
 * pushes a complete snapshot on connect and after every change; commands go
 * through the REST client instead. Returns a function that closes the socket.
 */
export function connectAdminSocket({ onState, onStatus }: AdminSocketHandlers): () => void {
  const socket: Socket = io("/admin", { path: "/socket.io", transports: ["websocket"] });

  socket.on("connect", () => onStatus("connected"));
  socket.on("disconnect", () => onStatus("disconnected"));
  socket.on("connect_error", () => onStatus("disconnected"));
  socket.io.on("reconnect_attempt", () => onStatus("connecting"));
  socket.on("state", onState);

  onStatus("connecting");
  return () => {
    socket.removeAllListeners();
    socket.io.removeAllListeners();
    socket.close();
  };
}
