package auth

import (
	"database/sql"
	"errors"
	"fmt"
	"path/filepath"
	"testing"
	"time"

	"playerhub/server/internal/validate"
)

func TestRolesAreUniqueWithoutCase(t *testing.T) {
	s := open(t, "")
	moderator, err := s.CreateRole("Moderator", 0x3366FF)
	if err != nil || moderator.ID == 0 {
		t.Fatalf("create: %+v %v", moderator, err)
	}
	if _, err := s.CreateRole("MODERATOR", 0); !errors.Is(err, ErrRoleNameTaken) {
		t.Fatalf("took a held name: %v", err)
	}
	author, _ := s.CreateRole("Author", 0xFF0000)
	if err := s.UpdateRole(Role{ID: author.ID, Name: "moderator", Color: 1}); !errors.Is(err, ErrRoleNameTaken) {
		t.Fatalf("renamed to a held name: %v", err)
	}
	if err := s.UpdateRole(Role{ID: moderator.ID, Name: "MODERATOR", Color: 0x00FF00}); err != nil {
		t.Fatalf("respelling: %v", err)
	}
	if err := s.UpdateRole(Role{ID: 999, Name: "Ghost"}); !errors.Is(err, ErrNoRole) {
		t.Fatalf("changed a missing role: %v", err)
	}
	roles, err := s.Roles(start)
	if err != nil || fmt.Sprint(roles) != fmt.Sprint([]RoleRecord{
		{Role: Role{moderator.ID, "MODERATOR", 0x00FF00}}, {Role: Role{author.ID, "Author", 0xFF0000}}}) {
		t.Fatalf("roles %+v %v", roles, err)
	}
}

func TestRolesAreLimited(t *testing.T) {
	s := open(t, "")
	for i := range validate.MaximumRoles {
		if _, err := s.CreateRole(fmt.Sprint("Role ", i), 0); err != nil {
			t.Fatalf("role %d: %v", i, err)
		}
	}
	if _, err := s.CreateRole("One more", 0); !errors.Is(err, ErrTooManyRoles) {
		t.Fatalf("over the limit: %v", err)
	}
}

func TestASessionHoldsOneRole(t *testing.T) {
	s := open(t, "")
	lydia, token := issue(t, s, "Lydia", start)
	serana, _ := issue(t, s, "Serana", start)
	stale, _ := issue(t, s, "Jordis", start.Add(-2*month))
	moderator, _ := s.CreateRole("Moderator", 0x3366FF)
	author, _ := s.CreateRole("Author", 0xFF0000)

	if err := s.SetRole(lydia.ID, 999, start); !errors.Is(err, ErrNoRole) {
		t.Fatalf("gave a missing role: %v", err)
	}
	for _, id := range []uint64{stale.ID, 999} {
		if err := s.SetRole(id, moderator.ID, start); !errors.Is(err, ErrNoSession) {
			t.Errorf("gave session %d a role: %v", id, err)
		}
	}
	for _, role := range []uint32{moderator.ID, author.ID} {
		if err := s.SetRole(lydia.ID, role, start); err != nil {
			t.Fatal(err)
		}
	}
	s.SetRole(serana.ID, moderator.ID, start)
	if resumed, _, err := s.Authenticate(token, "Lydia", start, nil); err != nil || resumed.Role != author.ID {
		t.Fatalf("resumed %+v %v", resumed, err)
	}
	roles, _ := s.Roles(start)
	if roles[0].Holders != 1 || roles[1].Holders != 1 {
		t.Fatalf("holders %+v", roles)
	}
	holders, total, err := s.Sessions("", author.ID, 0, 10, start)
	if err != nil || total != 1 || holders[0].ID != lydia.ID || holders[0].Role != author.ID {
		t.Fatalf("the role's holders: %+v %d %v", holders, total, err)
	}

	if err := s.SetRole(serana.ID, 0, start); err != nil {
		t.Fatal(err)
	}
	if deleted, err := s.DeleteRole(author.ID); !deleted || err != nil {
		t.Fatalf("delete: %v %v", deleted, err)
	}
	if deleted, _ := s.DeleteRole(author.ID); deleted {
		t.Fatal("deleted twice")
	}
	if resumed, _, _ := s.Authenticate(token, "Lydia", start, nil); resumed.Role != 0 {
		t.Fatalf("kept a deleted role: %+v", resumed)
	}
	if roles, _ := s.Roles(start); len(roles) != 1 || roles[0].Holders != 0 {
		t.Fatalf("after delete: %+v", roles)
	}
	next, _ := s.CreateRole("Author", 0)
	if next.ID <= author.ID {
		t.Fatalf("ID %d was reused after %d", next.ID, author.ID)
	}
}

func TestARoleDoesNotKeepItsSessionFromExpiring(t *testing.T) {
	s := open(t, "")
	lydia, _ := issue(t, s, "Lydia", start)
	moderator, _ := s.CreateRole("Moderator", 0)
	s.SetRole(lydia.ID, moderator.ID, start)
	later := start.Add(month + time.Hour)
	if roles, _ := s.Roles(later); roles[0].Holders != 0 {
		t.Fatalf("an expired session counts as a holder: %+v", roles)
	}
	if again, _ := issue(t, s, "Lydia", later); again.ID == lydia.ID || again.Role != 0 {
		t.Fatalf("got %+v", again)
	}
}

func TestAVersion3DatabaseGetsRoles(t *testing.T) {
	path := filepath.Join(t.TempDir(), "sessions.db")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	if _, err := db.Exec(`
CREATE TABLE sessions (
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	token_hash BLOB NOT NULL UNIQUE,
	display_name TEXT NOT NULL,
	display_name_key TEXT NOT NULL UNIQUE,
	created_at INTEGER NOT NULL,
	last_used_at INTEGER NOT NULL,
	resumed INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX sessions_last_used_at ON sessions (last_used_at);
CREATE TABLE bans (
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	session_id INTEGER NOT NULL,
	display_name TEXT NOT NULL,
	address TEXT,
	created_at INTEGER NOT NULL
);
CREATE INDEX bans_session_id ON bans (session_id);
CREATE INDEX bans_address ON bans (address);
INSERT INTO sessions (token_hash, display_name, display_name_key, created_at, last_used_at, resumed)
	VALUES (x'07', 'Lydia', 'lydia', ?, ?, 1);
PRAGMA user_version = 3;
`, start.Unix(), start.Unix()); err != nil {
		t.Fatal(err)
	}
	if err := db.Close(); err != nil {
		t.Fatal(err)
	}
	s := open(t, path)
	moderator, err := s.CreateRole("Moderator", 0x3366FF)
	if err != nil {
		t.Fatal(err)
	}
	if err := s.SetRole(1, moderator.ID, start); err != nil {
		t.Fatalf("the upgraded session takes no role: %v", err)
	}
	if records, _, _ := s.Sessions("", 0, 0, 10, start); len(records) != 1 || records[0].Role != moderator.ID {
		t.Fatalf("sessions %+v", records)
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	open(t, path)
}
