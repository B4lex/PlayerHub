package auth

import (
	"database/sql"
	"errors"
	"time"

	"playerhub/server/internal/validate"
)

var (
	ErrNoRole        = errors.New("no such role")
	ErrRoleNameTaken = errors.New("role name taken")
	ErrTooManyRoles  = errors.New("too many roles")
)

const rolesSchema = `
CREATE TABLE roles (
	-- AUTOINCREMENT: a deleted role's ID, which clients may still hold in chat lines, never
	-- comes to name another role.
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	name TEXT NOT NULL,
	name_key TEXT NOT NULL UNIQUE,
	-- 0xRRGGBB
	color INTEGER NOT NULL
);
-- NULL without a role.
ALTER TABLE sessions ADD COLUMN role_id INTEGER;
CREATE INDEX sessions_role_id ON sessions (role_id) WHERE role_id IS NOT NULL;
`

type Role struct {
	ID   uint32
	Name string
	// 0xRRGGBB
	Color uint32
}

type RoleRecord struct {
	Role
	// Live sessions holding the role.
	Holders int
}

func (s *Store) Roles(now time.Time) ([]RoleRecord, error) {
	rows, err := s.db.Query(`SELECT id, name, color, (SELECT COUNT(*) FROM sessions WHERE role_id = roles.id AND `+
		live+`) FROM roles ORDER BY id`, s.cutoff(now))
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	records := []RoleRecord{}
	for rows.Next() {
		var record RoleRecord
		if err := rows.Scan(&record.ID, &record.Name, &record.Color, &record.Holders); err != nil {
			return nil, err
		}
		records = append(records, record)
	}
	return records, rows.Err()
}

func (s *Store) CreateRole(name string, color uint32) (Role, error) {
	role := Role{Name: name, Color: color}
	err := s.transact(func(tx *sql.Tx) error {
		var count int
		if err := tx.QueryRow(`SELECT COUNT(*) FROM roles`).Scan(&count); err != nil {
			return err
		}
		if count >= validate.MaximumRoles {
			return ErrTooManyRoles
		}
		if err := requireFreeRoleName(tx, 0, name); err != nil {
			return err
		}
		result, err := tx.Exec(`INSERT INTO roles (name, name_key, color) VALUES (?, ?, ?)`,
			name, validate.RoleNameKey(name), color)
		if err != nil {
			return err
		}
		id, err := result.LastInsertId()
		role.ID = uint32(id)
		return err
	})
	if err != nil {
		return Role{}, err
	}
	return role, nil
}

func (s *Store) UpdateRole(role Role) error {
	return s.transact(func(tx *sql.Tx) error {
		if err := requireFreeRoleName(tx, role.ID, role.Name); err != nil {
			return err
		}
		result, err := tx.Exec(`UPDATE roles SET name = ?, name_key = ?, color = ? WHERE id = ?`,
			role.Name, validate.RoleNameKey(role.Name), role.Color, role.ID)
		if err != nil {
			return err
		}
		updated, err := result.RowsAffected()
		if err == nil && updated == 0 {
			err = ErrNoRole
		}
		return err
	})
}

// DeleteRole leaves the role's holders without a role.
func (s *Store) DeleteRole(id uint32) (bool, error) {
	deleted := false
	err := s.transact(func(tx *sql.Tx) error {
		result, err := tx.Exec(`DELETE FROM roles WHERE id = ?`, id)
		if err != nil {
			return err
		}
		count, err := result.RowsAffected()
		if err != nil || count == 0 {
			return err
		}
		deleted = true
		_, err = tx.Exec(`UPDATE sessions SET role_id = NULL WHERE role_id = ?`, id)
		return err
	})
	return deleted, err
}

// SetRole gives the session the role in place of its own; role 0 leaves it without one.
func (s *Store) SetRole(session uint64, role uint32, now time.Time) error {
	return s.transact(func(tx *sql.Tx) error {
		if err := s.purge(tx, now); err != nil {
			return err
		}
		var value any
		if role != 0 {
			var exists bool
			if err := tx.QueryRow(`SELECT EXISTS (SELECT 1 FROM roles WHERE id = ?)`, role).Scan(&exists); err != nil {
				return err
			}
			if !exists {
				return ErrNoRole
			}
			value = role
		}
		result, err := tx.Exec(`UPDATE sessions SET role_id = ? WHERE id = ?`, value, session)
		if err != nil {
			return err
		}
		updated, err := result.RowsAffected()
		if err == nil && updated == 0 {
			err = ErrNoSession
		}
		return err
	})
}

func requireFreeRoleName(tx *sql.Tx, own uint32, name string) error {
	var holder uint32
	err := tx.QueryRow(`SELECT id FROM roles WHERE name_key = ?`, validate.RoleNameKey(name)).Scan(&holder)
	switch {
	case errors.Is(err, sql.ErrNoRows):
		return nil
	case err != nil:
		return err
	case holder != own:
		return ErrRoleNameTaken
	}
	return nil
}
