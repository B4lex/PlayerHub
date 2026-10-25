import { useEffect, useState, type FormEvent } from "react";
import { api } from "../../api/client";
import type { Session, SessionPage } from "../../api/types";
import { Badge } from "../../components/Badge";
import { Button } from "../../components/Button";
import { ConfirmButton } from "../../components/ConfirmButton";
import { EmptyState } from "../../components/EmptyState";
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

function RenameForm({ session, onDone }: { session: Session; onDone: (renamed: boolean) => void }) {
  const [run, busy] = useAction();
  const [name, setName] = useState(session.displayName);
  const valid = DISPLAY_NAME.test(name);

  const submit = async (event: FormEvent) => {
    event.preventDefault();
    if (!valid) return;
    if (name === session.displayName) return onDone(false);
    if (await run(() => api.sessions.rename(session.id, name))) onDone(true);
  };

  return (
    <form className={styles.rename} onSubmit={submit}>
      <input
        autoFocus
        value={name}
        maxLength={15}
        aria-label={`New display name for ${session.displayName}`}
        aria-invalid={!valid}
        title="3 to 15 letters, digits, or underscores"
        onChange={(event) => setName(event.target.value)}
        onKeyDown={(event) => {
          if (event.key === "Escape") onDone(false);
        }}
      />
      <Button type="submit" size="sm" variant="primary" icon="check" busy={busy} disabled={!valid} aria-label="Save" />
      <Button size="sm" variant="ghost" icon="close" aria-label="Cancel" onClick={() => onDone(false)} />
    </form>
  );
}

function SessionRow({ session, now, onChanged }: { session: Session; now: number; onChanged: () => void }) {
  const [run, busy] = useAction();
  const [renaming, setRenaming] = useState(false);
  const connected = session.playerId !== null;

  const remove = async () => {
    await run(() => api.sessions.remove(session.id));
    onChanged();
  };

  return (
    <tr>
      <td>
        {renaming ? (
          <RenameForm
            session={session}
            onDone={(renamed) => {
              setRenaming(false);
              if (renamed) onChanged();
            }}
          />
        ) : (
          <div className={styles.name}>{session.displayName}</div>
        )}
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
      <td>
        <div className={styles.actions}>
          <Button
            size="sm"
            variant="ghost"
            disabled={connected || renaming}
            title={
              connected
                ? "A connected player keeps their name until they disconnect; kick them first"
                : "Give the session another display name; the player's next connection sets their own again"
            }
            onClick={() => setRenaming(true)}
          >
            Rename
          </Button>
          <ConfirmButton
            size="sm"
            variant="ghost"
            icon="trash"
            busy={busy}
            confirmLabel="Delete?"
            aria-label={`Delete session ${session.displayName}`}
            title={
              (connected ? "Disconnect the player and delete" : "Delete") +
              (session.banned ? " the session with its bans" : " the session") +
              "; their next connection gets a new session"
            }
            onConfirm={remove}
          />
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
                  <th scope="col">Session</th>
                  <th scope="col">Last used</th>
                  <th scope="col">Expires</th>
                  <th scope="col">Status</th>
                  <th scope="col">
                    <span className="visually-hidden">Actions</span>
                  </th>
                </tr>
              </thead>
              <tbody>
                {page.sessions.map((session) => (
                  <SessionRow key={session.id} session={session} now={now} onChanged={refresh} />
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
