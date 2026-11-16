import { useRef, useState } from "react";
import { Link } from "react-router-dom";
import { api } from "../../api/client";
import type { Role } from "../../api/types";
import { Button } from "../../components/Button";
import { ConfirmDialog } from "../../components/ConfirmDialog";
import { EmptyState } from "../../components/EmptyState";
import { Menu } from "../../components/Menu";
import { PageHeader } from "../../components/PageHeader";
import { Panel } from "../../components/Panel";
import { RoleBadge, RoleLetter } from "../../components/RoleBadge";
import { plural } from "../../lib/format";
import { roleChanged, roleCreated, roleRemoved } from "../../lib/predictions";
import { MAXIMUM_ROLE_NAME_LENGTH, validRoleName } from "../../lib/roles";
import { useServerState } from "../../state/ServerState";
import { useAction } from "../../state/useAction";
import styles from "./roles.module.css";

// The server's limit (validate.MaximumRoles).
const MAXIMUM_ROLES = 32;
const NEW_ROLE_COLOR = "#4f9dea";

/** Creates a role, or with `role` changes its name and color. */
function RoleDialog({ role, onClose }: { role?: Role; onClose: () => void }) {
  const { predict } = useServerState();
  const field = useRef<HTMLInputElement>(null);
  const [name, setName] = useState(role?.name ?? "");
  const [color, setColor] = useState(role?.color ?? NEW_ROLE_COLOR);
  const [busy, setBusy] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const trimmed = name.trim();
  const valid = validRoleName(trimmed);

  // Failures stay in the dialog, which covers the toasts, so the name can be corrected.
  const save = async () => {
    setBusy(true);
    try {
      if (role === undefined) {
        predict(roleCreated(await api.roles.create(trimmed, color))).succeeded();
      } else {
        const saved = await api.roles.update(role.id, trimmed, color);
        predict(roleChanged(role.id, saved.name, saved.color)).succeeded();
      }
      onClose();
    } catch (failure) {
      setError(failure instanceof Error ? failure.message : String(failure));
      setBusy(false);
    }
  };

  return (
    <ConfirmDialog
      title={role === undefined ? "New role" : `Change ${role.name}`}
      confirmLabel={role === undefined ? "Create" : "Save"}
      busy={busy}
      confirmDisabled={!valid || (role !== undefined && trimmed === role.name && color === role.color)}
      initialFocus={field}
      onConfirm={save}
      onClose={onClose}
    >
      <div className={styles.form}>
        <label htmlFor="role-name">Name</label>
        <input
          ref={field}
          id="role-name"
          className={styles.field}
          value={name}
          maxLength={MAXIMUM_ROLE_NAME_LENGTH}
          aria-invalid={name !== "" && !valid}
          title={`1 to ${MAXIMUM_ROLE_NAME_LENGTH} characters`}
          onChange={(event) => {
            setName(event.target.value);
            setError(null);
          }}
        />
        <label htmlFor="role-color">Color</label>
        <div className={styles.color}>
          <input
            id="role-color"
            className={styles.swatch}
            type="color"
            value={color}
            onChange={(event) => setColor(event.target.value)}
          />
          <span className="mono">{color}</span>
        </div>
        <span className={styles.muted}>In game</span>
        <span className={styles.preview}>
          {valid ? (
            <>
              <RoleLetter role={{ name: trimmed, color }} />
              <RoleBadge role={{ name: trimmed, color }} />
            </>
          ) : (
            <span className={styles.muted}>—</span>
          )}
        </span>
      </div>
      {error !== null && <p className={styles.error}>{error}</p>}
      <p>
        Holders show the letter by their name in chat, in the player list, and above their character, and the whole
        badge in their player menu. The game draws it on a dark background.
      </p>
    </ConfirmDialog>
  );
}

function RoleRow({ role }: { role: Role }) {
  const [run] = useAction();
  const [pending, setPending] = useState<"change" | "delete" | null>(null);
  const close = () => setPending(null);

  const remove = () => {
    close();
    void run(() => api.roles.remove(role.id), roleRemoved(role.id));
  };

  return (
    <tr>
      <td className={styles.menuCell}>
        <Menu
          label={`Actions for ${role.name}`}
          items={[
            { label: "Change", onSelect: () => setPending("change") },
            { label: "Delete", danger: true, onSelect: () => setPending("delete") },
          ]}
        />
        {pending === "change" && <RoleDialog role={role} onClose={close} />}
        {pending === "delete" && (
          <ConfirmDialog title={`Delete ${role.name}?`} confirmLabel="Delete" danger onConfirm={remove} onClose={close}>
            <p>
              {role.holders === 0
                ? "No player holds this role."
                : `${plural(role.holders, "player")} ${role.holders === 1 ? "holds" : "hold"} this role and will have none; connected ones lose the badge at once.`}
            </p>
          </ConfirmDialog>
        )}
      </td>
      <td>
        <span className={styles.preview}>
          <RoleLetter role={role} />
          <RoleBadge role={role} />
        </span>
      </td>
      <td className="mono">{role.color}</td>
      <td>
        {role.holders === 0 ? (
          <span className={styles.muted}>None</span>
        ) : (
          <Link to={`/sessions?role=${encodeURIComponent(role.id)}`}>{plural(role.holders, "player")}</Link>
        )}
      </td>
    </tr>
  );
}

/** The roles players can hold: a name and a color each, shown in game as a badge. */
export function RolesPage() {
  const { roles, loaded } = useServerState();
  const [creating, setCreating] = useState(false);
  const full = roles.length >= MAXIMUM_ROLES;

  return (
    <>
      <PageHeader
        title="Roles"
        description="A role is a name and a color. A player holds at most one, given on the Sessions page or the Dashboard, and the game shows it as a badge by their name."
      />

      <Panel
        title="Roles"
        description={loaded ? `${plural(roles.length, "role")} of at most ${MAXIMUM_ROLES}` : "Loading…"}
        actions={
          <Button
            variant="primary"
            icon="plus"
            disabled={!loaded || full}
            title={full ? `The server keeps at most ${MAXIMUM_ROLES} roles` : undefined}
            onClick={() => setCreating(true)}
          >
            New role
          </Button>
        }
        flush
      >
        {!loaded ? (
          <EmptyState icon="badge" title="Loading…" />
        ) : roles.length === 0 ? (
          <EmptyState icon="badge" title="No roles yet">
            Create a role such as Moderator or Supporter, then give it to a player.
          </EmptyState>
        ) : (
          <div className={styles.scroll}>
            <table className={styles.table}>
              <thead>
                <tr>
                  <th scope="col" className={styles.menuColumn}>
                    <span className="visually-hidden">Actions</span>
                  </th>
                  <th scope="col">Role</th>
                  <th scope="col">Color</th>
                  <th scope="col">Held by</th>
                </tr>
              </thead>
              <tbody>
                {roles.map((role) => (
                  <RoleRow key={role.id} role={role} />
                ))}
              </tbody>
            </table>
          </div>
        )}
      </Panel>
      {creating && <RoleDialog onClose={() => setCreating(false)} />}
    </>
  );
}
