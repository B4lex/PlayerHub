import { NavLink } from "react-router-dom";
import type { PageDefinition } from "../pages";
import { useLogin } from "../state/Login";
import { useServerState } from "../state/ServerState";
import { Brand } from "./Brand";
import { Icon } from "./Icon";
import styles from "./Sidebar.module.css";

const connectionLabel = {
  connected: "Live",
  connecting: "Connecting…",
  disconnected: "Server unreachable",
} as const;

export function Sidebar({ pages }: { pages: readonly PageDefinition[] }) {
  const { connection } = useServerState();
  const login = useLogin();
  return (
    <aside className={styles.sidebar}>
      <Brand />

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
        <a className={styles.docs} href="/api/state" target="_blank" rel="noreferrer">
          State as JSON
        </a>
        {login.required && (
          <button type="button" className={styles.signOut} onClick={() => void login.signOut()}>
            Log out
          </button>
        )}
      </div>
    </aside>
  );
}
