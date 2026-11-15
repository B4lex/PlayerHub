import { useCallback, useRef, useState } from "react";
import { useToast } from "../components/Toasts";
import type { Prediction } from "../lib/predictions";
import { useServerState } from "./ServerState";

type Run = <T>(command: () => Promise<T>, prediction?: Prediction) => Promise<T | undefined>;

/**
 * Run commands against the server, showing failures as a toast.
 *
 * Returns `[run, busy]`. `run` resolves with the command's result, or with
 * `undefined` after reporting a failure, so callers only handle success. With
 * a prediction the outcome shows at once and is taken back if the command
 * fails. Commands from one hook reach the server in the order they were run.
 * `busy` is true while any of them is in flight; it is for a control whose
 * outcome cannot be predicted, such as one that creates something.
 */
export function useAction(): [Run, boolean] {
  const toast = useToast();
  const { predict } = useServerState();
  const [inFlight, setInFlight] = useState(0);
  const last = useRef<Promise<unknown>>(Promise.resolve());

  const run = useCallback<Run>(
    async (command, prediction) => {
      const outcome = prediction && predict(prediction);
      setInFlight((count) => count + 1);
      const sent = last.current.then(command);
      last.current = sent.catch(() => undefined);
      try {
        const result = await sent;
        outcome?.succeeded();
        return result;
      } catch (error) {
        outcome?.failed();
        toast.error(error instanceof Error ? error.message : String(error));
        return undefined;
      } finally {
        setInFlight((count) => count - 1);
      }
    },
    [toast, predict],
  );

  return [run, inFlight > 0];
}
