import type { ReactNode } from "react";

export type IconName =
  | "dashboard"
  | "replay"
  | "play"
  | "pause"
  | "stop"
  | "record"
  | "restart"
  | "loop"
  | "trash"
  | "close"
  | "check"
  | "alert"
  | "users"
  | "film"
  | "clock"
  | "disk"
  | "gauge"
  | "key"
  | "more";

// 24x24 outline icons; `fill` ones are solid shapes.
const glyphs: Record<IconName, { fill?: boolean; body: ReactNode }> = {
  dashboard: {
    body: (
      <>
        <rect x="3" y="3" width="7" height="9" rx="1.6" />
        <rect x="14" y="3" width="7" height="5" rx="1.6" />
        <rect x="14" y="12" width="7" height="9" rx="1.6" />
        <rect x="3" y="16" width="7" height="5" rx="1.6" />
      </>
    ),
  },
  replay: {
    body: (
      <>
        <path d="M3.5 12a8.5 8.5 0 1 0 2.6-6.1" />
        <path d="M3.5 4.5v4.6h4.6" />
        <path d="M10.2 9l4.6 3-4.6 3z" />
      </>
    ),
  },
  play: { fill: true, body: <path d="M7 4.6v14.8a.6.6 0 0 0 .92.5l11.6-7.4a.6.6 0 0 0 0-1L7.92 4.1A.6.6 0 0 0 7 4.6z" /> },
  pause: {
    fill: true,
    body: (
      <>
        <rect x="6" y="4.5" width="4.2" height="15" rx="1.1" />
        <rect x="13.8" y="4.5" width="4.2" height="15" rx="1.1" />
      </>
    ),
  },
  stop: { fill: true, body: <rect x="5.5" y="5.5" width="13" height="13" rx="2" /> },
  record: { fill: true, body: <circle cx="12" cy="12" r="6.5" /> },
  restart: {
    body: (
      <>
        <path d="M4 12a8 8 0 1 0 2.5-5.8" />
        <path d="M4 4.5v4.4h4.4" />
      </>
    ),
  },
  loop: {
    body: (
      <>
        <path d="M17 2.5l3.5 3.5L17 9.5" />
        <path d="M3.5 11V9.5A3.5 3.5 0 0 1 7 6h13.5" />
        <path d="M7 21.5L3.5 18 7 14.5" />
        <path d="M20.5 13v1.5A3.5 3.5 0 0 1 17 18H3.5" />
      </>
    ),
  },
  trash: {
    body: (
      <>
        <path d="M4 7h16" />
        <path d="M9 7V4.5h6V7" />
        <path d="M6.5 7l.9 12.2a1.5 1.5 0 0 0 1.5 1.3h6.2a1.5 1.5 0 0 0 1.5-1.3L17.5 7" />
        <path d="M10 11v6M14 11v6" />
      </>
    ),
  },
  close: { body: <path d="M6 6l12 12M18 6L6 18" /> },
  check: { body: <path d="M4.5 12.5l5 5 10-11" /> },
  alert: {
    body: (
      <>
        <path d="M12 3.5l9.5 16.5h-19z" />
        <path d="M12 10v4.5M12 17.6v.1" />
      </>
    ),
  },
  users: {
    body: (
      <>
        <circle cx="9" cy="8" r="3.4" />
        <path d="M2.5 20a6.5 6.5 0 0 1 13 0" />
        <path d="M16 4.8a3.4 3.4 0 0 1 0 6.4M18.5 14.2A6.5 6.5 0 0 1 21.5 20" />
      </>
    ),
  },
  film: {
    body: (
      <>
        <rect x="3" y="4" width="18" height="16" rx="2.4" />
        <path d="M8 4v16M16 4v16M3 9h5M3 15h5M16 9h5M16 15h5" />
      </>
    ),
  },
  clock: {
    body: (
      <>
        <circle cx="12" cy="12" r="9" />
        <path d="M12 7v5.2l3.4 2" />
      </>
    ),
  },
  disk: {
    body: (
      <>
        <ellipse cx="12" cy="6" rx="8" ry="3" />
        <path d="M4 6v6c0 1.7 3.6 3 8 3s8-1.3 8-3V6" />
        <path d="M4 12v6c0 1.7 3.6 3 8 3s8-1.3 8-3v-6" />
      </>
    ),
  },
  gauge: {
    body: (
      <>
        <path d="M3.5 17.5a8.5 8.5 0 1 1 17 0" />
        <path d="M12 17.5l4.3-5.3" />
        <circle cx="12" cy="17.5" r="1.4" />
      </>
    ),
  },
  key: {
    body: (
      <>
        <circle cx="8" cy="15" r="4.5" />
        <path d="M11.2 11.8L20 3M16.5 6.5l2.5 2.5M14 9l2 2" />
      </>
    ),
  },
  more: {
    fill: true,
    body: (
      <>
        <circle cx="12" cy="5.5" r="1.8" />
        <circle cx="12" cy="12" r="1.8" />
        <circle cx="12" cy="18.5" r="1.8" />
      </>
    ),
  },
};

interface IconProps {
  name: IconName;
  size?: number;
  className?: string;
}

/** Decorative by default: put a label on the control that contains the icon. */
export function Icon({ name, size = 20, className }: IconProps) {
  const glyph = glyphs[name];
  return (
    <svg
      className={className}
      width={size}
      height={size}
      viewBox="0 0 24 24"
      fill={glyph.fill ? "currentColor" : "none"}
      stroke={glyph.fill ? "none" : "currentColor"}
      strokeWidth="1.8"
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
      focusable="false"
    >
      {glyph.body}
    </svg>
  );
}
