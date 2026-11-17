import { createContext, useCallback, useContext, useEffect, useMemo, useState, type ReactNode } from "react";
import { api, setUnauthorizedHandler } from "../api/client";
import { LoginPage } from "../features/login/LoginPage";

/**
 * "unknown" is a server that could not be asked: the panel is shown, and the feed's next refusal
 * asks again.
 */
type LoginState = "checking" | "signedOut" | "signedIn" | "open" | "unknown";

interface LoginValue {
  /** The server has a password, so the panel can sign out. */
  required: boolean;
  signOut: () => Promise<void>;
  /** Ask the server whether the panel is still signed in, after a refusal it could not read. */
  recheck: () => void;
}

const LoginContext = createContext<LoginValue | null>(null);

/** Shows the login page in place of its children while the server wants a password. */
export function LoginGate({ children }: { children: ReactNode }) {
  const [state, setState] = useState<LoginState>("checking");

  const recheck = useCallback(() => {
    api.login.status().then(
      (status) => setState(!status.required ? "open" : status.signedIn ? "signedIn" : "signedOut"),
      () => setState((current) => (current === "checking" ? "unknown" : current)),
    );
  }, []);

  useEffect(() => {
    recheck();
    setUnauthorizedHandler(() => setState("signedOut"));
    return () => setUnauthorizedHandler(null);
  }, [recheck]);

  const signOut = useCallback(async () => {
    await api.login.signOut();
    setState("signedOut");
  }, []);

  const value = useMemo<LoginValue>(
    () => ({ required: state === "signedIn", signOut, recheck }),
    [state, signOut, recheck],
  );

  if (state === "checking") return null;
  if (state === "signedOut") return <LoginPage onSignedIn={() => setState("signedIn")} />;
  return <LoginContext.Provider value={value}>{children}</LoginContext.Provider>;
}

export function useLogin(): LoginValue {
  const value = useContext(LoginContext);
  if (value === null) throw new Error("useLogin must be used inside LoginGate");
  return value;
}
