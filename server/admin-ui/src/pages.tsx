import type { ReactNode } from "react";
import type { IconName } from "./components/Icon";
import { DashboardPage } from "./features/dashboard/DashboardPage";
import { ReplaysPage } from "./features/replays/ReplaysPage";

export interface PageDefinition {
  id: string;
  /** Route path, relative to the app's base. */
  path: string;
  /** Sidebar label. */
  label: string;
  icon: IconName;
  element: ReactNode;
}

/**
 * Every page of the panel. The sidebar and the routes are both built from this
 * list, so adding a page (players, server settings, logs...) is one entry here
 * plus its feature folder.
 */
export const pages: readonly PageDefinition[] = [
  { id: "dashboard", path: "/", label: "Dashboard", icon: "dashboard", element: <DashboardPage /> },
  { id: "replays", path: "/replays", label: "Replay management", icon: "replay", element: <ReplaysPage /> },
];
