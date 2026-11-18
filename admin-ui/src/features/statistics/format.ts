/** "45 s", "12 min", "1 h 20 min", "36 h". */
export function formatDuration(seconds: number): string {
  if (seconds < 60) return `${Math.round(seconds)} s`;
  const minutes = Math.round(seconds / 60);
  if (minutes < 60) return `${minutes} min`;
  const hours = Math.floor(minutes / 60);
  if (hours >= 24) return `${Math.round(seconds / 3600).toLocaleString()} h`;
  const rest = minutes % 60;
  return rest === 0 ? `${hours} h` : `${hours} h ${rest} min`;
}

/** Bytes per second as network bandwidth: "840 kbit/s", "12.4 Mbit/s". */
export function formatBandwidth(bytesPerSecond: number): string {
  const bits = bytesPerSecond * 8;
  if (bits < 1_000_000) return `${Math.round(bits / 1000).toLocaleString()} kbit/s`;
  const megabits = bits / 1_000_000;
  return `${megabits.toFixed(megabits < 100 ? 1 : 0)} Mbit/s`;
}

export function formatPercent(fraction: number): string {
  const percent = fraction * 100;
  return `${percent < 10 && percent !== 0 ? percent.toFixed(1) : Math.round(percent)}%`;
}

/** A count that may be an average: whole numbers stay whole. */
export function formatCount(value: number): string {
  return Number.isInteger(value) ? value.toLocaleString() : value.toFixed(1);
}

/** The bucket starting at `startMs`: a day as a date, shorter steps as a date and time span. */
export function formatBucket(startMs: number, endMs: number, stepMinutes: number): string {
  const start = new Date(startMs);
  if (stepMinutes >= 1440) {
    return start.toLocaleDateString(undefined, { weekday: "short", month: "short", day: "numeric" });
  }
  const time = { hour: "2-digit", minute: "2-digit", hourCycle: "h23" } as const;
  const day = start.toLocaleDateString(undefined, { month: "short", day: "numeric" });
  return `${day}, ${start.toLocaleTimeString(undefined, time)}–${new Date(endMs).toLocaleTimeString(undefined, time)}`;
}

export function formatMoment(ms: number): string {
  return new Date(ms).toLocaleString(undefined, {
    month: "short",
    day: "numeric",
    hour: "2-digit",
    minute: "2-digit",
    hourCycle: "h23",
  });
}

const disconnectLabels: Record<string, string> = {
  left: "Left",
  dropped: "Connection lost",
  timeout: "Stopped answering",
  kicked: "Kicked",
  banned: "Banned",
  replaced: "Connected again elsewhere",
  rate_limited: "Sent too fast",
  backlog: "Could not keep up",
  server_stopped: "Server stopped",
  server_lost: "Server crashed",
};

const refusalLabels: Record<string, string> = {
  joining_busy: "Too many joining at once",
  no_hello: "Never said hello",
  invalid_hello: "Invalid hello",
  protocol_mismatch: "Outdated client or server",
  display_name_invalid: "Invalid display name",
  display_name_taken: "Display name taken",
  banned: "Banned",
  too_many_new_sessions: "Too many new sessions",
  server_full: "Server full",
  address_full: "Too many from one address",
  server_error: "Server error",
};

export function disconnectLabel(reason: string): string {
  return disconnectLabels[reason] ?? reason;
}

export function refusalLabel(reason: string): string {
  return refusalLabels[reason] ?? reason;
}
