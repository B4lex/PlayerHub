import type { ReactNode } from "react";
import styles from "./EmptyState.module.css";
import { Icon, type IconName } from "./Icon";

interface EmptyStateProps {
  icon: IconName;
  title: string;
  children?: ReactNode;
}

export function EmptyState({ icon, title, children }: EmptyStateProps) {
  return (
    <div className={styles.empty}>
      <Icon name={icon} size={28} className={styles.icon} />
      <p className={styles.title}>{title}</p>
      {children && <p className={styles.text}>{children}</p>}
    </div>
  );
}
