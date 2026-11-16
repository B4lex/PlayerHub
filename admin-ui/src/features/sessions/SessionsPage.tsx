import { useEffect, useRef, useState } from "react";
import { useSearchParams } from "react-router-dom";
import { api } from "../../api/client";
import type { Ban, Role, Session, SessionPage } from "../../api/types";
import { Badge } from "../../components/Badge";
import { Button } from "../../components/Button";
import { ConfirmDialog } from "../../components/ConfirmDialog";
import { EmptyState } from "../../components/EmptyState";
import { Menu, type MenuItem } from "../../components/Menu";
import { PageHeader } from "../../components/PageHeader";
import { Panel } from "../../components/Panel";
import { RoleBadge } from "../../components/RoleBadge";
import { Spinner } from "../../components/Spinner";
import { formatDate, formatDateTime, formatRelative, plural } from "../../lib/format";
import { useTicker } from "../../lib/useTicker";
import { useServerState } from "../../state/ServerState";
import { useAction } from "../../state/useAction";
import { SetRoleDialog } from "../roles/SetRoleDialog";
import styles from "./SessionsPage.module.css";

const PAGE_SIZE = 50;
const SEARCH_DELAY_MS = 250;
// The server's rule (validate.DisplayName).
const DISPLAY_NAME = /^[A-Za-z0-9_]{3,15}$/;

/** A command's outcome shown on its row before a fetched page carries it; null hides the row. */
type RowChange = Partial<Session> | null;

interface HeldChange {
  token: number;
  sessionId: string;
  change: RowChange;
  /** Counts the settled commands up to this one's answer; null while the command is in flight. */
  settledAt: number | null;
}

/** The query a page of sessions answers. */
interface LoadedPage extends SessionPage {
  search: string;
  role: string | null;
  offset: number;
}

function iso(ms: number): string {
  return new Date(ms).toISOString();
}

function RenameDialog({ session, onClose, onRenamed }: { session: Session; onClose: () => void; onRenamed: (displayName: string) => void }) {
  const field = useRef<HTMLInputElement>(null);
  const [name, setName] = useState(session.displayName);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const valid = DISPLAY_NAME.test(name);

  // Failures stay in the dialog, which covers the toasts, so the name can be corrected.
  const rename = async () => {
    setBusy(true);
    try {
      onRenamed((await api.sessions.rename(session.id, name)).displayName);
    } catch (failure) {
      setError(failure instanceof Error ? failure.message : String(failure));
      setBusy(false);
    }
  };

  return (
    <ConfirmDialog
      title={`Rename ${session.displayName}`}
      confirmLabel="Rename"
      busy={busy}
      confirmDisabled={!valid || name === session.displayName}
      initialFocus={field}
      onConfirm={rename}
      onClose={onClose}
    >
      <input
        ref={field}
        className={styles.nameField}
        value={name}
        maxLength={15}
        aria-label={`New display name for ${session.displayName}`}
        aria-invalid={!valid}
        title="3 to 15 letters, digits, or underscores"
        onChange={(event) => {
          setName(event.target.value);
          setError(null);
        }}
      />
      {error !== null && <p className={styles.error}>{error}</p>}
      <p>The player's next connection sets their own name again.</p>
    </ConfirmDialog>
  );
}

type SessionCommand = "rename" | "role" | "lift" | "delete";

interface SessionRowProps {
  session: Session;
  /** The session's role, if it holds one the server still has. */
  role: Role | undefined;
  bans: Ban[];
  now: number;
  /** Shows a change on the row at once; tell the returned function whether its command succeeded. */
  onChange: (change: RowChange) => (succeeded: boolean) => void;
}

function SessionRow({ session, role, bans, now, onChange }: SessionRowProps) {
  const [run] = useAction();
  const [pending, setPending] = useState<SessionCommand | null>(null);
  const connected = session.playerId !== null;
  const close = () => setPending(null);

  const send = async (change: RowChange, command: () => Promise<unknown>) => {
    close();
    const settle = onChange(change);
    const done = await run(async () => {
      await command();
      return true;
    });
    settle(done === true);
  };

  const remove = () => send(null, () => api.sessions.remove(session.id));

  // A session collects one ban per ban command, with or without its address.
  const lift = () => send({ banned: false }, () => Promise.all(bans.map((ban) => api.bans.lift(ban.id))));

  const items: MenuItem[] = [
    {
      label: "Rename",
      disabled: connected,
      title: connected ? "A connected player keeps their name until they disconnect; kick them first" : undefined,
      onSelect: () => setPending("rename"),
    },
    { label: "Set role", onSelect: () => setPending("role") },
  ];
  if (session.banned) items.push({ label: "Lift ban", disabled: bans.length === 0, onSelect: () => setPending("lift") });
  items.push({ label: "Delete", danger: true, onSelect: () => setPending("delete") });

  return (
    <tr>
      <td className={styles.menuCell}>
        <Menu label={`Actions for ${session.displayName}`} items={items} />
        {pending === "rename" && (
          <RenameDialog
            session={session}
            onClose={close}
            onRenamed={(displayName) => {
              close();
              onChange({ displayName })(true);
            }}
          />
        )}
        {pending === "role" && (
          <SetRoleDialog
            name={session.displayName}
            roleId={role?.id ?? null}
            onClose={close}
            onSet={(roleId) => send({ roleId }, () => api.sessions.setRole(session.id, roleId))}
          />
        )}
        {pending === "lift" && (
          <ConfirmDialog title={`Lift the ban on ${session.displayName}?`} confirmLabel="Lift ban" onConfirm={lift} onClose={close}>
            <p>
              The session can connect again
              {bans.some((ban) => ban.address !== null) ? ", and so can everyone from the address it was banned with." : "."}
            </p>
          </ConfirmDialog>
        )}
        {pending === "delete" && (
          <ConfirmDialog title={`Delete ${session.displayName}?`} confirmLabel="Delete" danger onConfirm={remove} onClose={close}>
            <p>
              {connected && "The player is disconnected. "}
              {session.banned && "The session's bans are deleted with it. "}
              Their next connection gets a new session.
            </p>
          </ConfirmDialog>
        )}
      </td>
      <td>
        <div className={styles.name}>{session.displayName}</div>
        <div className={`${styles.sub} mono`}>session {session.id}</div>
      </td>
      <td>{role !== undefined && <RoleBadge role={role} />}</td>
      <td className={styles.when}>
        <time dateTime={iso(session.lastUsedAtMs)} title={formatDateTime(iso(session.lastUsedAtMs))}>
          {connected ? "now" : formatRelative(iso(session.lastUsedAtMs), now)}
        </time>
        <div className={styles.sub}>
          created{" "}
          <time dateTime={iso(session.createdAtMs)} title={formatDateTime(iso(session.createdAtMs))}>
            {formatDate(session.createdAtMs)}
          </time>
        </div>
      </td>
      <td className={styles.when}>
        {session.expiresAtMs === null && session.banned ? (
          <span className={styles.sub} title="A banned session is kept until its ban is lifted">
            never
          </span>
        ) : session.expiresAtMs === null ? (
          // A ban just lifted: the page fetched next carries the new expiry.
          <span className={styles.sub}>—</span>
        ) : connected ? (
          <span className={styles.sub} title="A session does not expire while connected">
            —
          </span>
        ) : (
          <time dateTime={iso(session.expiresAtMs)} title={formatDateTime(iso(session.expiresAtMs))}>
            {formatDate(session.expiresAtMs)}
          </time>
        )}
      </td>
      <td>
        <div className={styles.badges}>
          {connected && (
            <Badge tone="success" title={`Connected as player ${session.playerId}`}>
              Online
            </Badge>
          )}
          {session.banned && <Badge tone="danger">Banned</Badge>}
        </div>
      </td>
    </tr>
  );
}

/** Lists the sessions the server knows, with renaming and deleting. */
export function SessionsPage() {
  const { players, bans, roles } = useServerState();
  const [parameters, setParameters] = useSearchParams();
  // In the address, so the Roles page can link to a role's holders.
  const role = parameters.get("role");
  useTicker(30_000); // re-render so relative times ("5 min ago") stay current
  const now = Date.now();
  const [searchInput, setSearchInput] = useState("");
  const [search, setSearch] = useState("");
  const [offset, setOffset] = useState(0);
  const [revision, setRevision] = useState(0);
  const [page, setPage] = useState<LoadedPage | null>(null);
  const [error, setError] = useState<string | null>(null);
  const [held, setHeld] = useState<HeldChange[]>([]);
  const tokens = useRef(0);
  const settledCommands = useRef(0);

  // Connections, bans, and roles given elsewhere change a row, and the feed reports them.
  const membership = [
    players.filter((player) => !player.virtual).map((player) => player.id).join(","),
    bans.map((ban) => ban.id).join(","),
    roles.map((other) => `${other.id}:${other.holders}`).join(","),
  ].join("|");

  useEffect(() => {
    const timer = window.setTimeout(() => {
      setSearch(searchInput.trim());
      setOffset(0);
    }, SEARCH_DELAY_MS);
    return () => window.clearTimeout(timer);
  }, [searchInput]);

  useEffect(() => {
    let current = true;
    // A page requested before a command answered may not show its outcome, so that change stays held.
    const settledBefore = settledCommands.current;
    api.sessions.list(search, role, offset, PAGE_SIZE).then(
      (loaded) => {
        if (!current) return;
        // Deleting the last row of the last page leaves an empty page behind.
        if (loaded.sessions.length === 0 && offset > 0) {
          setOffset(Math.max(0, Math.floor((loaded.total - 1) / PAGE_SIZE) * PAGE_SIZE));
          return;
        }
        setPage({ ...loaded, search, role, offset });
        setHeld((changes) => changes.filter((change) => change.settledAt === null || change.settledAt > settledBefore));
        setError(null);
      },
      (failure: unknown) => {
        if (current) setError(failure instanceof Error ? failure.message : String(failure));
      },
    );
    return () => {
      current = false;
    };
  }, [search, role, offset, revision, membership]);

  const hold = (sessionId: string, change: RowChange) => {
    const token = ++tokens.current;
    setHeld((changes) => [...changes, { token, sessionId, change, settledAt: null }]);
    return (succeeded: boolean) => {
      const settledAt = ++settledCommands.current;
      setHeld((changes) =>
        succeeded
          ? changes.map((entry) => (entry.token === token ? { ...entry, settledAt } : entry))
          : changes.filter((entry) => entry.token !== token),
      );
      setRevision((value) => value + 1);
    };
  };

  const sessions = (page?.sessions ?? []).flatMap((session) => {
    let shown = session;
    for (const { sessionId, change } of held) {
      if (sessionId !== session.id) continue;
      if (change === null) return [];
      shown = { ...shown, ...change };
    }
    return [shown];
  });
  const shown = sessions.length;
  const total = (page?.total ?? 0) - ((page?.sessions.length ?? 0) - shown);
  // The rows on screen answer an earlier search or page.
  const waiting = page !== null && (page.search !== searchInput.trim() || page.role !== role || page.offset !== offset);
  const filtered = page !== null && (page.search !== "" || page.role !== null);

  const filterByRole = (id: string) => {
    setParameters(id === "" ? {} : { role: id }, { replace: true });
    setOffset(0);
  };

  return (
    <>
      <PageHeader
        title="Sessions"
        description="Every player identity this server keeps: its display name, when it was last used, and when it expires."
      />

      <Panel
        title="Sessions"
        description={
          page === null
            ? "Loading…"
            : !filtered
              ? `${plural(total, "session")}, most recently used first`
              : page.search === ""
                ? `${plural(total, "session")} with this role`
                : `${plural(total, "session")} matching “${page.search}”`
        }
        actions={
          <>
            {waiting && <Spinner className={styles.waitingMark} />}
            {(roles.length > 0 || role !== null) && (
              <select
                className={styles.roleFilter}
                aria-label="Show sessions with a role"
                value={role ?? ""}
                onChange={(event) => filterByRole(event.target.value)}
              >
                <option value="">Any role</option>
                {role !== null && !roles.some((other) => other.id === role) && <option value={role}>Deleted role</option>}
                {roles.map((other) => (
                  <option key={other.id} value={other.id}>
                    {other.name}
                  </option>
                ))}
              </select>
            )}
            <input
            className={styles.search}
            type="search"
            placeholder="Name or session ID"
            aria-label="Search sessions"
            value={searchInput}
            onChange={(event) => setSearchInput(event.target.value)}
          />
          </>
        }
        flush
      >
        {error !== null && page === null ? (
          <EmptyState icon="alert" title="Cannot load sessions">
            {error}
          </EmptyState>
        ) : page === null ? (
          <EmptyState icon="key" title="Loading…" />
        ) : shown === 0 ? (
          <EmptyState icon="key" title={!filtered ? "No sessions yet" : "No matching sessions"}>
            {!filtered
              ? "A session is issued the first time a player connects with a display name."
              : page.search === ""
                ? "No session holds this role."
                : "Search matches part of a display name, or a whole session ID."}
          </EmptyState>
        ) : (
          <>
            <table className={waiting ? `${styles.table} ${styles.waiting}` : styles.table} aria-busy={waiting || undefined}>
              <thead>
                <tr>
                  <th scope="col" className={styles.menuColumn}>
                    <span className="visually-hidden">Actions</span>
                  </th>
                  <th scope="col">Session</th>
                  <th scope="col">Role</th>
                  <th scope="col">Last used</th>
                  <th scope="col">Expires</th>
                  <th scope="col">Status</th>
                </tr>
              </thead>
              <tbody>
                {sessions.map((session) => (
                  <SessionRow
                    key={session.id}
                    session={session}
                    role={roles.find((other) => other.id === session.roleId)}
                    bans={bans.filter((ban) => ban.sessionId === session.id)}
                    now={now}
                    onChange={(change) => hold(session.id, change)}
                  />
                ))}
              </tbody>
            </table>
            {total > PAGE_SIZE && (
              <div className={styles.pager}>
                <span className="tabular">
                  {offset + 1}–{offset + shown} of {total}
                </span>
                <Button size="sm" variant="ghost" disabled={offset === 0} onClick={() => setOffset(Math.max(0, offset - PAGE_SIZE))}>
                  Previous
                </Button>
                <Button size="sm" variant="ghost" disabled={offset + shown >= total} onClick={() => setOffset(offset + PAGE_SIZE)}>
                  Next
                </Button>
              </div>
            )}
          </>
        )}
      </Panel>
    </>
  );
}
