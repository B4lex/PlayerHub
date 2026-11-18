import styles from "./BarList.module.css";

export interface BarItem {
  key: string;
  label: string;
  value: number;
}

/** Labeled horizontal bars of one measure, each with its value at the tip. */
export function BarList({ items, format = (v) => v.toLocaleString() }: { items: BarItem[]; format?: (value: number) => string }) {
  const top = Math.max(1, ...items.map((item) => item.value));
  return (
    <ul className={styles.list}>
      {items.map((item) => (
        <li key={item.key} className={styles.row}>
          <span className={styles.label} title={item.label}>
            {item.label}
          </span>
          <span className={styles.track}>
            <span className={styles.bar} style={{ width: `${(item.value / top) * 100}%` }} />
            <span className="tabular">{format(item.value)}</span>
          </span>
        </li>
      ))}
    </ul>
  );
}
