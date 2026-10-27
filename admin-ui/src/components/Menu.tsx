import { useEffect, useId, useRef, useState, type KeyboardEvent, type ToggleEvent } from "react";
import { createPortal } from "react-dom";
import { Button } from "./Button";
import type { IconName } from "./Icon";
import styles from "./Menu.module.css";

export interface MenuItem {
  label: string;
  onSelect: () => void;
  danger?: boolean;
  disabled?: boolean;
  title?: string;
}

interface MenuProps {
  /** The trigger's accessible name. */
  label: string;
  icon?: IconName;
  items: MenuItem[];
}

const GAP_PX = 4;

/** An icon button that opens a list of commands, closing on a choice, Escape, a click outside, or scrolling. */
export function Menu({ label, icon = "more", items }: MenuProps) {
  const id = useId();
  const trigger = useRef<HTMLButtonElement>(null);
  const list = useRef<HTMLDivElement>(null);
  const [open, setOpen] = useState(false);

  // The list is placed once from the trigger, so it would drift away from it on scroll.
  useEffect(() => {
    if (!open) return;
    const close = () => list.current?.hidePopover();
    window.addEventListener("scroll", close, { capture: true, passive: true });
    window.addEventListener("resize", close);
    return () => {
      window.removeEventListener("scroll", close, { capture: true });
      window.removeEventListener("resize", close);
    };
  }, [open]);

  const place = (event: ToggleEvent<HTMLDivElement>) => {
    if (event.newState !== "open" || !trigger.current) return;
    const anchor = trigger.current.getBoundingClientRect();
    const style = event.currentTarget.style;
    style.left = `${anchor.left}px`;
    // Opens upward near the bottom of the window; the list's height is unknown until it is shown.
    if (anchor.bottom > window.innerHeight * 0.6) {
      style.top = "auto";
      style.bottom = `${window.innerHeight - anchor.top + GAP_PX}px`;
    } else {
      style.top = `${anchor.bottom + GAP_PX}px`;
      style.bottom = "auto";
    }
  };

  const toggled = (event: ToggleEvent<HTMLDivElement>) => {
    const opened = event.newState === "open";
    setOpen(opened);
    if (opened) event.currentTarget.querySelector<HTMLButtonElement>("button:not(:disabled)")?.focus();
  };

  const navigate = (event: KeyboardEvent<HTMLDivElement>) => {
    const step = { ArrowDown: 1, ArrowUp: -1 }[event.key];
    if (step === undefined) return;
    event.preventDefault();
    const buttons = [...event.currentTarget.querySelectorAll<HTMLButtonElement>("button:not(:disabled)")];
    if (buttons.length === 0) return;
    const current = buttons.indexOf(document.activeElement as HTMLButtonElement);
    const next = current < 0 ? (step > 0 ? 0 : buttons.length - 1) : (current + step + buttons.length) % buttons.length;
    buttons[next]?.focus();
  };

  const select = (item: MenuItem) => {
    list.current?.hidePopover();
    item.onSelect();
  };

  return (
    <>
      <Button
        ref={trigger}
        size="sm"
        variant="ghost"
        icon={icon}
        aria-label={label}
        aria-haspopup="menu"
        aria-expanded={open}
        popoverTarget={id}
      />
      {createPortal(
        // In the body, so the list inherits nothing from where the trigger sits, such as a table cell's styles.
        <div
          ref={list}
          id={id}
          popover="auto"
          role="menu"
          aria-label={label}
          className={styles.menu}
          onBeforeToggle={place}
          onToggle={toggled}
          onKeyDown={navigate}
        >
          {items.map((item) => (
            <button
              key={item.label}
              type="button"
              role="menuitem"
              className={item.danger ? `${styles.item} ${styles.danger}` : styles.item}
              disabled={item.disabled}
              title={item.title}
              onClick={() => select(item)}
            >
              {item.label}
            </button>
          ))}
        </div>,
        document.body,
      )}
    </>
  );
}
