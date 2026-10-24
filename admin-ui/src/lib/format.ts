import type { FormRef } from "../api/types";

/** `1:05` or `1:05.3`; hours only when needed. */
export function formatTimestamp(ms: number, tenths = false): string {
  // Integer arithmetic: deriving the fraction from float seconds shows 65.3 s as 65.2.
  const totalMs = Math.floor(Math.max(0, ms));
  const whole = Math.floor(totalMs / 1000);
  const hours = Math.floor(whole / 3600);
  const minutes = Math.floor((whole % 3600) / 60);
  const seconds = whole % 60;
  const fraction = tenths ? `.${Math.floor((totalMs % 1000) / 100)}` : "";
  const ss = String(seconds).padStart(2, "0");
  return hours > 0
    ? `${hours}:${String(minutes).padStart(2, "0")}:${ss}${fraction}`
    : `${minutes}:${ss}${fraction}`;
}

export function formatBytes(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  const units = ["KB", "MB", "GB"];
  let value = bytes / 1024;
  let unit = 0;
  while (value >= 1024 && unit < units.length - 1) {
    value /= 1024;
    unit++;
  }
  return `${value.toFixed(value < 10 ? 1 : 0)} ${units[unit]}`;
}

/** "3 min ago", "2 h ago", or a date once it is more than a day old. */
export function formatRelative(iso: string, now = Date.now()): string {
  const then = Date.parse(iso);
  if (Number.isNaN(then)) return iso;
  const seconds = Math.round((now - then) / 1000);
  if (seconds < 45) return "just now";
  if (seconds < 3600) return `${Math.round(seconds / 60)} min ago`;
  if (seconds < 86400) return `${Math.round(seconds / 3600)} h ago`;
  return new Date(then).toLocaleDateString(undefined, { month: "short", day: "numeric", year: "numeric" });
}

export function formatDateTime(iso: string): string {
  const then = new Date(iso);
  return Number.isNaN(then.getTime()) ? iso : then.toLocaleString();
}

/** A Skyrim form ID as the game shows it. */
export function formatFormRef(ref: FormRef): string {
  return `${ref.plugin}:0x${ref.formId.toString(16).toUpperCase().padStart(6, "0")}`;
}

export function shortId(id: string): string {
  return id.length > 10 ? `${id.slice(0, 8)}…` : id;
}

export function plural(count: number, singular: string, pluralForm = `${singular}s`): string {
  return `${count} ${count === 1 ? singular : pluralForm}`;
}
