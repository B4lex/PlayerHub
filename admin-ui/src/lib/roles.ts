/** The server's rule for a role name (validate.RoleName): 1 to 24 characters, no space at either end. */
export const MAXIMUM_ROLE_NAME_LENGTH = 24;

export function validRoleName(name: string): boolean {
  const length = [...name].length;
  return length >= 1 && length <= MAXIMUM_ROLE_NAME_LENGTH && name.trim() === name && !/[\u0000-\u001f\u007f]/.test(name);
}

/** The letter the game shows by a holder's name: the role name's first character in upper case. */
export function roleLetter(name: string): string {
  const [first = ""] = name;
  return first.toUpperCase();
}

/** The game draws a role as a chat chip (UI::ChatStyle): its color at these alphas for the fill and the outline, and at full for the text. */
const CHIP_FILL_ALPHA = 0.18;
const CHIP_OUTLINE_ALPHA = 0.55;

export interface RoleChipColors {
  fill: string;
  outline: string;
  text: string;
}

/** The chip's colors for the "#rrggbb" color. */
export function roleChipColors(color: string): RoleChipColors {
  const value = Number.parseInt(color.slice(1), 16);
  const rgb = `${(value >> 16) & 0xff} ${(value >> 8) & 0xff} ${value & 0xff}`;
  return { fill: `rgb(${rgb} / ${CHIP_FILL_ALPHA})`, outline: `rgb(${rgb} / ${CHIP_OUTLINE_ALPHA})`, text: color };
}
