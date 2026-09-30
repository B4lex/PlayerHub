import type { ReactNode } from "react";
import styles from "./Badge.module.css";

interface BadgeProps {
  tone?: "neutral" | "accent" | "success" | "warning" | "danger" | "info";
  /** A leading dot; `pulse` makes it blink to show something live. */
  dot?: boolean;
  pulse?: boolean;
  title?: string;
  children: ReactNode;
}

export function Badge({ tone = "neutral", dot = false, pulse = false, title, children }: BadgeProps) {
  return (
    <span className={`${styles.badge} ${styles[tone]}`} title={title}>
      {dot && <span className={`${styles.dot} ${pulse ? styles.pulse : ""}`} aria-hidden="true" />}
      {children}
    </span>
  );
}
