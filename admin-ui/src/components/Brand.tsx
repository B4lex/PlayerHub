import styles from "./Brand.module.css";

export function Brand() {
  return (
    <div className={styles.brand}>
      <span className={styles.logo} aria-hidden="true" />
      <div>
        <div className={styles.name}>PlayerHub</div>
        <div className={styles.subtitle}>Server admin</div>
      </div>
    </div>
  );
}
