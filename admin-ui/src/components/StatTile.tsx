import type { ReactNode } from "react";
import styles from "./StatTile.module.css";
import { Icon, type IconName } from "./Icon";

interface StatTileProps {
  icon: IconName;
  label: string;
  value: ReactNode;
  hint?: ReactNode;
  /** Highlights the tile, for something that is happening right now. */
  live?: boolean;
}

export function StatTile({ icon, label, value, hint, live = false }: StatTileProps) {
  return (
    <div className={`${styles.tile} ${live ? styles.live : ""}`}>
      <div className={styles.label}>
        <Icon name={icon} size={16} />
        {label}
      </div>
      <div className={`${styles.value} tabular`}>{value}</div>
      {hint && <div className={styles.hint}>{hint}</div>}
    </div>
  );
}
