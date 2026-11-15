import styles from "./Spinner.module.css";

/** Marks a wait. It stays invisible for the first moments, so a quick answer shows no spinner at all. */
export function Spinner({ className }: { className?: string }) {
  return <span className={className ? `${styles.spinner} ${className}` : styles.spinner} aria-hidden="true" />;
}
