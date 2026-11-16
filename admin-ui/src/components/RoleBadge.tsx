import type { CSSProperties } from "react";
import { roleChipColors, roleLetter } from "../lib/roles";
import styles from "./RoleBadge.module.css";

interface RoleLook {
  name: string;
  /** "#rrggbb". */
  color: string;
}

function chipStyle(color: string): CSSProperties {
  const colors = roleChipColors(color);
  return {
    backgroundImage: `linear-gradient(${colors.fill}, ${colors.fill})`,
    borderColor: colors.outline,
    color: colors.text,
  };
}

/** A role as the game's player menu shows it: its name in a chip of its color. */
export function RoleBadge({ role }: { role: RoleLook }) {
  return (
    <span className={styles.badge} style={chipStyle(role.color)}>
      {role.name}
    </span>
  );
}

/** A role as the game shows it by a holder's name: its first letter in a chip of its color. */
export function RoleLetter({ role }: { role: RoleLook }) {
  return (
    <span className={styles.letter} style={chipStyle(role.color)} title={role.name}>
      {roleLetter(role.name)}
    </span>
  );
}
