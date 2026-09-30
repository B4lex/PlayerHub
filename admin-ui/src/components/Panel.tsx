import type { ReactNode } from "react";
import styles from "./Panel.module.css";

interface PanelProps {
  title: string;
  description?: ReactNode;
  /** Controls shown at the right of the header. */
  actions?: ReactNode;
  /** Remove the body padding so a table can run edge to edge. */
  flush?: boolean;
  children: ReactNode;
}

export function Panel({ title, description, actions, flush = false, children }: PanelProps) {
  return (
    <section className={styles.panel}>
      <header className={styles.header}>
        <div className={styles.heading}>
          <h2 className={styles.title}>{title}</h2>
          {description && <p className={styles.description}>{description}</p>}
        </div>
        {actions && <div className={styles.actions}>{actions}</div>}
      </header>
      <div className={flush ? styles.flush : styles.body}>{children}</div>
    </section>
  );
}
