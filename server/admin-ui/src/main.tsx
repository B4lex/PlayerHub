import { StrictMode } from "react";
import { createRoot } from "react-dom/client";
import { BrowserRouter } from "react-router-dom";
import { App } from "./App";
import { ToastProvider } from "./components/Toasts";
import { ServerStateProvider } from "./state/ServerState";
import "./styles/tokens.css";
import "./styles/base.css";

const root = document.getElementById("root");
if (root === null) throw new Error("missing #root element");

createRoot(root).render(
  <StrictMode>
    {/* The game server hosts the built app under /admin. */}
    <BrowserRouter basename={import.meta.env.BASE_URL.replace(/\/$/, "")}>
      <ToastProvider>
        <ServerStateProvider>
          <App />
        </ServerStateProvider>
      </ToastProvider>
    </BrowserRouter>
  </StrictMode>,
);
