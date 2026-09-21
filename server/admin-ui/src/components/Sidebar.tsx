import { NavLink } from "react-router-dom";
import type { PageDefinition } from "../pages";
import { useServerState } from "../state/ServerState";
import { Icon } from "./Icon";
import styles from "./Sidebar.module.css";

const connectionLabel = {
  connected: "Live",
  connecting: "Connecting…",
  disconnected: "Server unreachable",
} as const;

export function Sidebar({ pages }: { pages: readonly PageDefinition[] }) {
  const { connection } = useServerState();
  return (
    <aside className={styles.sidebar}>
      <div className={styles.brand}>
        <span className={styles.logo} aria-hidden="true" />
        <div>
          <div className={styles.name}>Skyrim MP</div>
          <div className={styles.subtitle}>Server admin</div>
        </div>
      </div>

      <nav className={styles.nav} aria-label="Main">
        {pages.map((page) => (
          <NavLink
            key={page.id}
            to={page.path}
            end={page.path === "/"}
            className={({ isActive }) => `${styles.link} ${isActive ? styles.active : ""}`}
          >
            <Icon name={page.icon} size={19} />
            <span>{page.label}</span>
          </NavLink>
        ))}
      </nav>

      <div className={styles.footer}>
        <div className={`${styles.connection} ${styles[connection]}`} role="status">
          <span className={styles.dot} aria-hidden="true" />
          {connectionLabel[connection]}
        </div>
        <a className={styles.docs} href="/docs" target="_blank" rel="noreferrer">
          API reference
        </a>
      </div>
    </aside>
  );
}
