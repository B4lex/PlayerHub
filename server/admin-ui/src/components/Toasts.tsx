import {
  createContext,
  useCallback,
  useContext,
  useMemo,
  useRef,
  useState,
  type ReactNode,
} from "react";
import styles from "./Toasts.module.css";
import { Icon } from "./Icon";

type ToastTone = "error" | "success";

interface Toast {
  id: number;
  tone: ToastTone;
  message: string;
}

interface ToastApi {
  error: (message: string) => void;
  success: (message: string) => void;
}

const ToastContext = createContext<ToastApi | null>(null);

export function ToastProvider({ children }: { children: ReactNode }) {
  const [toasts, setToasts] = useState<Toast[]>([]);
  const nextId = useRef(1);

  const dismiss = useCallback((id: number) => {
    setToasts((current) => current.filter((toast) => toast.id !== id));
  }, []);

  const push = useCallback(
    (tone: ToastTone, message: string) => {
      const id = nextId.current++;
      setToasts((current) => [...current.slice(-3), { id, tone, message }]);
      window.setTimeout(() => dismiss(id), tone === "error" ? 8000 : 4000);
    },
    [dismiss],
  );

  const api = useMemo<ToastApi>(
    () => ({ error: (message) => push("error", message), success: (message) => push("success", message) }),
    [push],
  );

  return (
    <ToastContext.Provider value={api}>
      {children}
      <div className={styles.region} aria-live="polite">
        {toasts.map((toast) => (
          <div
            key={toast.id}
            className={`${styles.toast} ${styles[toast.tone]}`}
            role={toast.tone === "error" ? "alert" : "status"}
          >
            <Icon name={toast.tone === "error" ? "alert" : "check"} size={18} />
            <span className={styles.message}>{toast.message}</span>
            <button className={styles.dismiss} onClick={() => dismiss(toast.id)} aria-label="Dismiss">
              <Icon name="close" size={16} />
            </button>
          </div>
        ))}
      </div>
    </ToastContext.Provider>
  );
}

export function useToast(): ToastApi {
  const value = useContext(ToastContext);
  if (value === null) throw new Error("useToast must be used inside ToastProvider");
  return value;
}
