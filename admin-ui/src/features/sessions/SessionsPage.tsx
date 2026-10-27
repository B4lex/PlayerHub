import { useEffect, useRef, useState } from "react";
import { api } from "../../api/client";
import type { Ban, Session, SessionPage } from "../../api/types";
import { Badge } from "../../components/Badge";
import { Button } from "../../components/Button";
import { ConfirmDialog } from "../../components/ConfirmDialog";
import { EmptyState } from "../../components/EmptyState";
import { Menu, type MenuItem } from "../../components/Menu";
import { PageHeader } from "../../components/PageHeader";
import { Panel } from "../../components/Panel";
import { formatDate, formatDateTime, formatRelative, plural } from "../../lib/format";
import { useTicker } from "../../lib/useTicker";
import { useServerState } from "../../state/ServerState";
import { useAction } from "../../state/useAction";
import styles from "./SessionsPage.module.css";

const PAGE_SIZE = 50;
const SEARCH_DELAY_MS = 250;
// The server's rule (validate.DisplayName).
const DISPLAY_NAME = /^[A-Za-z0-9_]{3,15}$/;

function iso(ms: number): string {
  return new Date(ms).toISOString();
}

function RenameDialog({ session, onClose, onRenamed }: { session: Session; onClose: () => void; onRenamed: () => void }) {
  const field = useRef<HTMLInputElement>(null);
  const [name, setName] = useState(session.displayName);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const valid = DISPLAY_NAME.test(name);

  // Failures stay in the dialog, which covers the toasts, so the name can be corrected.
  const rename = async () => {
    setBusy(true);
    try {
      await api.sessions.rename(session.id, name);
      onRenamed();
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

type SessionCommand = "rename" | "lift" | "delete";

function SessionRow({ session, bans, now, onChanged }: { session: Session; bans: Ban[]; now: number; onChanged: () => void }) {
  const [run, busy] = useAction();
  const [pending, setPending] = useState<SessionCommand | null>(null);
  const connected = session.playerId !== null;
  const close = () => setPending(null);

  const remove = async () => {
    await run(() => api.sessions.remove(session.id));
    close();
    onChanged();
  };

  // A session collects one ban per ban command, with or without its address.
  const lift = async () => {
    await run(() => Promise.all(bans.map((ban) => api.bans.lift(ban.id))));
    close();
    onChanged();
  };

  const items: MenuItem[] = [
    {
      label: "Rename",
      disabled: connected,
      title: connected ? "A connected player keeps their name until they disconnect; kick them first" : undefined,
      onSelect: () => setPending("rename"),
    },
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
            onRenamed={() => {
              close();
              onChanged();
            }}
          />
        )}
        {pending === "lift" && (
          <ConfirmDialog title={`Lift the ban on ${session.displayName}?`} confirmLabel="Lift ban" busy={busy} onConfirm={lift} onClose={close}>
            <p>
              The session can connect again
              {bans.some((ban) => ban.address !== null) ? ", and so can everyone from the address it was banned with." : "."}
            </p>
          </ConfirmDialog>
        )}
        {pending === "delete" && (
          <ConfirmDialog title={`Delete ${session.displayName}?`} confirmLabel="Delete" danger busy={busy} onConfirm={remove} onClose={close}>
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
        {session.expiresAtMs === null ? (
          <span className={styles.sub} title="A banned session is kept until its ban is lifted">
            never
          </span>
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
  const { players, bans } = useServerState();
  useTicker(30_000); // re-render so relative times ("5 min ago") stay current
  const now = Date.now();
  const [searchInput, setSearchInput] = useState("");
  const [search, setSearch] = useState("");
  const [offset, setOffset] = useState(0);
  const [revision, setRevision] = useState(0);
  const [page, setPage] = useState<SessionPage | null>(null);
  const [error, setError] = useState<string | null>(null);

  // Connections and bans change a row's status, and the feed reports both.
  const membership = [
    players.filter((player) => !player.virtual).map((player) => player.id).join(","),
    bans.map((ban) => ban.id).join(","),
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
    api.sessions.list(search, offset, PAGE_SIZE).then(
      (loaded) => {
        if (!current) return;
        // Deleting the last row of the last page leaves an empty page behind.
        if (loaded.sessions.length === 0 && offset > 0) {
          setOffset(Math.max(0, Math.floor((loaded.total - 1) / PAGE_SIZE) * PAGE_SIZE));
          return;
        }
        setPage(loaded);
        setError(null);
      },
      (failure: unknown) => {
        if (current) setError(failure instanceof Error ? failure.message : String(failure));
      },
    );
    return () => {
      current = false;
    };
  }, [search, offset, revision, membership]);

  const refresh = () => setRevision((value) => value + 1);
  const total = page?.total ?? 0;
  const shown = page?.sessions.length ?? 0;

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
            : search === ""
              ? `${plural(total, "session")}, most recently used first`
              : `${plural(total, "session")} matching “${search}”`
        }
        actions={
          <input
            className={styles.search}
            type="search"
            placeholder="Name or session ID"
            aria-label="Search sessions"
            value={searchInput}
            onChange={(event) => setSearchInput(event.target.value)}
          />
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
          <EmptyState icon="key" title={search === "" ? "No sessions yet" : "No matching sessions"}>
            {search === ""
              ? "A session is issued the first time a player connects with a display name."
              : "Search matches part of a display name, or a whole session ID."}
          </EmptyState>
        ) : (
          <>
            <table className={styles.table}>
              <thead>
                <tr>
                  <th scope="col" className={styles.menuColumn}>
                    <span className="visually-hidden">Actions</span>
                  </th>
                  <th scope="col">Session</th>
                  <th scope="col">Last used</th>
                  <th scope="col">Expires</th>
                  <th scope="col">Status</th>
                </tr>
              </thead>
              <tbody>
                {page.sessions.map((session) => (
                  <SessionRow
                    key={session.id}
                    session={session}
                    bans={bans.filter((ban) => ban.sessionId === session.id)}
                    now={now}
                    onChanged={refresh}
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
