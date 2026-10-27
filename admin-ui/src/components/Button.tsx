import type { ButtonHTMLAttributes, ReactNode, Ref } from "react";
import styles from "./Button.module.css";
import { Icon, type IconName } from "./Icon";

interface ButtonProps extends ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: "primary" | "secondary" | "ghost" | "danger";
  size?: "sm" | "md";
  icon?: IconName;
  /** Shows a spinner and blocks clicks while a command runs. */
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
  const classes = [styles.button, styles[variant], styles[size], children ? "" : styles.iconOnly, className]
    .filter(Boolean)
    .join(" ");
  return (
    <button {...rest} type={type} className={classes} disabled={disabled || busy} aria-busy={busy || undefined}>
      {busy ? <span className={styles.spinner} aria-hidden="true" /> : icon && <Icon name={icon} size={size === "sm" ? 16 : 18} />}
      {children}
    </button>
  );
}
