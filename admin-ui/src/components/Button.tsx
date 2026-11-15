import type { ButtonHTMLAttributes, ReactNode, Ref } from "react";
import styles from "./Button.module.css";
import { Icon, type IconName } from "./Icon";
import { Spinner } from "./Spinner";

interface ButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: "primary" | "secondary" | "ghost" | "danger";
  size?: "sm" | "md";
  icon?: IconName;
  /** Blocks clicks while a command runs, and shows a spinner in place of the icon or label if it takes long. */
  busy?: boolean;
  children?: ReactNode;
  ref?: Ref<HTMLButtonElement>;
}

export function Button({
  variant = "secondary",
  size = "md",
  icon,
  busy = false,
  children,
  className,
  disabled,
  type = "button",
  ...rest
}: ButtonProps) {
  const classes = [
    styles.button,
    styles[variant],
    styles[size],
    children ? "" : styles.iconOnly,
    busy ? styles.busy : "",
    className,
  ]
    .filter(Boolean)
    .join(" ");
  return (
    <button {...rest} type={type} className={classes} disabled={disabled || busy} aria-busy={busy || undefined}>
      {(icon || busy) && (
        <span className={icon ? styles.glyph : `${styles.glyph} ${styles.over}`}>
          {icon && <Icon name={icon} size={size === "sm" ? 16 : 18} />}
          {busy && <Spinner />}
        </span>
      )}
      {children && <span className={icon ? undefined : styles.label}>{children}</span>}
    </button>
  );
}
