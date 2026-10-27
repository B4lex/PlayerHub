import { useEffect, useId, useRef, type ReactNode, type RefObject } from "react";
import { createPortal } from "react-dom";
import { Button } from "./Button";
import styles from "./ConfirmDialog.module.css";

interface ConfirmDialogProps {
  title: string;
  confirmLabel: string;
  danger?: boolean;
  /** Keeps the dialog open, its confirm button spinning, while the command runs. */
  busy?: boolean;
  confirmDisabled?: boolean;
  /** Focused when the dialog opens instead of the cancel button, such as a field to fill in. */
  initialFocus?: RefObject<HTMLElement | null>;
  onConfirm: () => void;
  onClose: () => void;
  children?: ReactNode;
}

/**
 * A modal question, shown while it is mounted. Escape and a click outside cancel it; Enter in a field confirms.
 * It renders into the body so it inherits nothing from where it is used, such as a table cell's nowrap.
 */
export function ConfirmDialog({
  title,
  confirmLabel,
  danger = false,
  busy = false,
  confirmDisabled = false,
  initialFocus,
  onConfirm,
  onClose,
  children,
}: ConfirmDialogProps) {
  const titleId = useId();
  const dialog = useRef<HTMLDialogElement>(null);
  const cancel = useRef<HTMLButtonElement>(null);

  useEffect(() => {
    const element = dialog.current;
    if (!element) return;
    element.showModal();
    // Destructive questions start on the safe answer.
    (initialFocus?.current ?? cancel.current)?.focus();
    return () => element.close();
  }, []);

  return createPortal(
    <dialog
      ref={dialog}
      className={styles.dialog}
      aria-labelledby={titleId}
      onCancel={(event) => {
        event.preventDefault();
        if (!busy) onClose();
      }}
      onClick={(event) => {
        if (event.target === event.currentTarget && !busy) onClose();
      }}
    >
      <form
        className={styles.content}
        onSubmit={(event) => {
          event.preventDefault();
          if (!busy && !confirmDisabled) onConfirm();
        }}
      >
        <h2 id={titleId} className={styles.title}>
          {title}
        </h2>
        {children && <div className={styles.body}>{children}</div>}
        <div className={styles.actions}>
          <Button ref={cancel} variant="ghost" disabled={busy} onClick={onClose}>
            Cancel
          </Button>
          <Button type="submit" variant={danger ? "danger" : "primary"} busy={busy} disabled={confirmDisabled}>
            {confirmLabel}
          </Button>
        </div>
      </form>
    </dialog>,
    document.body,
  );
}
