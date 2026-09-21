import { useEffect, useState, type ComponentProps } from "react";
import { Button } from "./Button";

type ConfirmButtonProps = Omit<ComponentProps<typeof Button>, "onClick"> & {
  /** Runs only after a second click within a few seconds of the first. */
  onConfirm: () => void;
  confirmLabel?: string;
};

/** A destructive action that asks to be clicked twice instead of opening a dialog. */
export function ConfirmButton({ onConfirm, confirmLabel = "Confirm", children, ...rest }: ConfirmButtonProps) {
  const [armed, setArmed] = useState(false);

  useEffect(() => {
    if (!armed) return;
    const timer = window.setTimeout(() => setArmed(false), 3000);
    return () => window.clearTimeout(timer);
  }, [armed]);

  return (
    <Button
      {...rest}
      variant={armed ? "danger" : rest.variant}
      onClick={() => {
        if (armed) {
          setArmed(false);
          onConfirm();
        } else {
          setArmed(true);
        }
      }}
      onBlur={() => setArmed(false)}
    >
      {armed ? confirmLabel : children}
    </Button>
  );
}
