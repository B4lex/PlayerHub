import react from "@vitejs/plugin-react";
import { loadEnv } from "vite";
import { defineConfig } from "vitest/config";

// The built app is served by the game server at /admin. In development, Vite serves
// it on its own port and forwards the API and Socket.IO to the running game server
// (override with ADMIN_SERVER_URL, e.g. when the server is not on port 3000).
export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, ".", "");
  const server = env["ADMIN_SERVER_URL"] ?? "http://127.0.0.1:3000";
  return {
    base: "/admin/",
    plugins: [react()],
    server: {
      proxy: {
        "/api": server,
        "/socket.io": { target: server, ws: true },
      },
    },
    test: { environment: "node" },
  };
});
