import { useCallback, useState } from "react";
import { useToast } from "../components/Toasts";

/**
 * Run commands against the server, showing failures as a toast.
 *
 * Returns `[run, busy]`. `run` resolves with the command's result, or with
 * `undefined` after reporting a failure, so callers only handle success.
 * `busy` is true while any command started through this hook is in flight; it
 * is for disabling the control that started it.
 */
export function useAction(): [<T>(command: () => Promise<T>) => Promise<T | undefined>, boolean] {
  const toast = useToast();
  const [inFlight, setInFlight] = useState(0);

  const run = useCallback(
    async <T,>(command: () => Promise<T>): Promise<T | undefined> => {
      setInFlight((count) => count + 1);
      try {
        return await command();
      } catch (error) {
        toast.error(error instanceof Error ? error.message : String(error));
        return undefined;
      } finally {
        setInFlight((count) => count - 1);
      }
    },
    [toast],
  );

  return [run, inFlight > 0];
}
