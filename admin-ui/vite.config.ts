import react from "@vitejs/plugin-react";
import { loadEnv } from "vite";
import { defineConfig } from "vitest/config";

// The built app is served by the game server's admin address at /admin. In development,
// Vite serves it on its own port and forwards the API and its event feed to that address
// (override with ADMIN_SERVER_URL, e.g. when admin_listen is not port 3001).
export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, ".", "");
  const server = env["ADMIN_SERVER_URL"] ?? "http://127.0.0.1:3001";
  return {
    base: "/admin/",
    plugins: [react()],
    server: {
      proxy: {
        "/api": server,
      },
    },
    test: { environment: "node" },
  };
});
