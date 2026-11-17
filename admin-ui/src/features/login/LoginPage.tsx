import { useState, type FormEvent } from "react";
import { api, ApiError } from "../../api/client";
import { Brand } from "../../components/Brand";
import { Button } from "../../components/Button";
import styles from "./LoginPage.module.css";

export function LoginPage({ onSignedIn }: { onSignedIn: () => void }) {
  const [password, setPassword] = useState("");
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);

  const submit = async (event: FormEvent) => {
    event.preventDefault();
    if (busy || password === "") return;
    setBusy(true);
    setError(null);
    try {
      await api.login.signIn(password);
      onSignedIn();
    } catch (failure) {
      setError(failure instanceof ApiError ? failure.message : String(failure));
      setBusy(false);
    }
  };

  return (
    <main className={styles.page}>
      <form className={styles.card} onSubmit={submit}>
        <Brand />
        <h1 className={styles.title}>Log in</h1>
        {/* Lets password managers file the password under a name. */}
        <input type="text" name="username" autoComplete="username" value="admin" readOnly hidden />
        <label className={styles.field}>
          Password
          <input
            type="password"
            name="password"
            autoComplete="current-password"
            autoFocus
            value={password}
            onChange={(event) => setPassword(event.target.value)}
            aria-invalid={error !== null || undefined}
            aria-describedby={error === null ? undefined : "login-error"}
          />
        </label>
        {error !== null && (
          <p id="login-error" className={styles.error} role="alert">
            {error}
          </p>
        )}
        <Button type="submit" variant="primary" busy={busy} disabled={password === ""}>
          Log in
        </Button>
      </form>
    </main>
  );
}
