import { useRef, useState } from "react";
import { Link } from "react-router-dom";
import { ConfirmDialog } from "../../components/ConfirmDialog";
import { RoleBadge, RoleLetter } from "../../components/RoleBadge";
import { useServerState } from "../../state/ServerState";
import styles from "./roles.module.css";

interface SetRoleDialogProps {
  /** Whose role it is, for the title. */
  name: string;
  roleId: string | null;
  onClose: () => void;
  /** Called with the choice when it differs from `roleId`; the caller sends it and closes the dialog. */
  onSet: (roleId: string | null) => void;
}

/** Picks the one role a session holds, or none. */
export function SetRoleDialog({ name, roleId, onClose, onSet }: SetRoleDialogProps) {
  const { roles } = useServerState();
  const field = useRef<HTMLSelectElement>(null);
  // A role deleted while the dialog is open reads as none.
  const [chosen, setChosen] = useState(roleId ?? "");
  const role = roles.find((other) => other.id === chosen);
  const choice = role === undefined ? null : role.id;

  return (
    <ConfirmDialog
      title={`Role of ${name}`}
      confirmLabel="Set role"
      confirmDisabled={choice === roleId}
      initialFocus={field}
      onConfirm={() => onSet(choice)}
      onClose={onClose}
    >
      <div className={styles.pick}>
        <select
          ref={field}
          className={styles.field}
          value={role === undefined ? "" : role.id}
          aria-label={`Role of ${name}`}
          onChange={(event) => setChosen(event.target.value)}
        >
          <option value="">No role</option>
          {roles.map((other) => (
            <option key={other.id} value={other.id}>
              {other.name}
            </option>
          ))}
        </select>
        {role !== undefined && (
          <span className={styles.preview}>
            <RoleLetter role={role} />
            <RoleBadge role={role} />
          </span>
        )}
      </div>
      {roles.length === 0 ? (
        <p>
          There are no roles yet. Create one on the <Link to="/roles">Roles</Link> page.
        </p>
      ) : (
        <p>A player holds one role. In game it shows as a badge by their name, at once if they are connected.</p>
      )}
    </ConfirmDialog>
  );
}
