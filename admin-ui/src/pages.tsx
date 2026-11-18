import type { ReactNode } from "react";
import type { IconName } from "./components/Icon";
import { DashboardPage } from "./features/dashboard/DashboardPage";
import { LoadTestPage } from "./features/loadtest/LoadTestPage";
import { ReplaysPage } from "./features/replays/ReplaysPage";
import { RolesPage } from "./features/roles/RolesPage";
import { SessionsPage } from "./features/sessions/SessionsPage";
import { StatisticsPage } from "./features/statistics/StatisticsPage";

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
  { id: "statistics", path: "/statistics", label: "Statistics", icon: "chart", element: <StatisticsPage /> },
  { id: "sessions", path: "/sessions", label: "Sessions", icon: "key", element: <SessionsPage /> },
  { id: "roles", path: "/roles", label: "Roles", icon: "badge", element: <RolesPage /> },
  { id: "replays", path: "/replays", label: "Replay management", icon: "replay", element: <ReplaysPage /> },
  { id: "load-testing", path: "/load-testing", label: "Load testing", icon: "gauge", element: <LoadTestPage /> },
];
