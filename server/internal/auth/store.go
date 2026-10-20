package auth

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"database/sql"
	"errors"
	"fmt"
	"net/netip"
	"time"

	_ "modernc.org/sqlite"

	"playerhub/server/internal/validate"
)

var (
	ErrDisplayNameTaken = errors.New("display name taken")
	ErrIssueRefused     = errors.New("too many new sessions")
	ErrBanned           = errors.New("banned")
)

const schemaVersion = 3

const schema = `
CREATE TABLE sessions (
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	token_hash BLOB NOT NULL UNIQUE,
	display_name TEXT NOT NULL,
	display_name_key TEXT NOT NULL UNIQUE,
	created_at INTEGER NOT NULL,
	last_used_at INTEGER NOT NULL,
	-- The token came back in a hello, so its client holds it.
	resumed INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX sessions_last_used_at ON sessions (last_used_at);
` + bansSchema

const bansSchema = `
CREATE TABLE bans (
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	session_id INTEGER NOT NULL,
	display_name TEXT NOT NULL,
	-- A network prefix, or NULL when the ban leaves the address open.
	address TEXT,
	created_at INTEGER NOT NULL
);
CREATE INDEX bans_session_id ON bans (session_id);
CREATE INDEX bans_address ON bans (address);
`

const upgradeFromVersion1 = `
ALTER TABLE sessions RENAME COLUMN nickname TO display_name;
ALTER TABLE sessions RENAME COLUMN nickname_key TO display_name_key;
` + bansSchema

// Banned sessions never expire, so their token keeps matching the ban.
const unbanned = `id NOT IN (SELECT session_id FROM bans)`

type Session struct {
	ID          uint64
	DisplayName string
}

type Ban struct {
	ID          uint64
	Session     uint64
	DisplayName string
	// Zero when the ban leaves the address open.
	Address   netip.Prefix
	CreatedAt time.Time
}

type Store struct {
	db       *sql.DB
	lifetime time.Duration
}

func Open(path string, lifetime time.Duration) (*Store, error) {
	if path == "" {
		path = ":memory:"
	}
	db, err := sql.Open("sqlite", path+"?_pragma=busy_timeout(5000)&_pragma=journal_mode(WAL)&_pragma=synchronous(NORMAL)")
	if err != nil {
		return nil, err
	}
	// A second connection to an in-memory database would open another one.
	db.SetMaxOpenConns(1)
	s := &Store{db: db, lifetime: lifetime}
	if err := s.migrate(); err != nil {
		db.Close()
		return nil, err
	}
	return s, nil
}

func (s *Store) Close() error {
	return s.db.Close()
}

func (s *Store) migrate() error {
	var version int
	if err := s.db.QueryRow(`PRAGMA user_version`).Scan(&version); err != nil {
		return err
	}
	var statements string
	switch version {
	case schemaVersion:
		return nil
	case 0:
		statements = schema
	case 1:
		statements = upgradeFromVersion1
	case 2:
		statements = bansSchema
	default:
		return fmt.Errorf("session database has schema %d, this server knows %d", version, schemaVersion)
	}
	return s.transact(func(tx *sql.Tx) error {
		if _, err := tx.Exec(statements); err != nil {
			return err
		}
		_, err := tx.Exec(fmt.Sprintf(`PRAGMA user_version = %d`, schemaVersion))
		return err
	})
}

func (s *Store) Authenticate(token []byte, displayName string, now time.Time, mayIssue func() bool) (Session, []byte, error) {
	var session Session
	var issued []byte
	err := s.transact(func(tx *sql.Tx) error {
		if err := s.purge(tx, now); err != nil {
			return err
		}
		if len(token) != 0 {
			hash := sha256.Sum256(token)
			var current string
			var banned bool
			err := tx.QueryRow(`SELECT id, display_name, NOT (`+unbanned+`) FROM sessions WHERE token_hash = ?`, hash[:]).
				Scan(&session.ID, &current, &banned)
			switch {
			case err == nil && banned:
				return ErrBanned
			case err == nil:
				session.DisplayName = displayName
				if current != displayName {
					if err := rename(tx, session.ID, displayName); err != nil {
						return err
					}
				}
				_, err = tx.Exec(`UPDATE sessions SET last_used_at = ?, resumed = 1 WHERE id = ?`, now.Unix(), session.ID)
				return err
			case !errors.Is(err, sql.ErrNoRows):
				return err
			}
		}
		if err := requireFree(tx, 0, displayName); err != nil {
			return err
		}
		if mayIssue != nil && !mayIssue() {
			return ErrIssueRefused
		}
		issued = make([]byte, validate.SessionTokenBytes)
		if _, err := rand.Read(issued); err != nil {
			return err
		}
		hash := sha256.Sum256(issued)
		result, err := tx.Exec(
			`INSERT INTO sessions (token_hash, display_name, display_name_key, created_at, last_used_at) VALUES (?, ?, ?, ?, ?)`,
			hash[:], displayName, validate.DisplayNameKey(displayName), now.Unix(), now.Unix())
		if err != nil {
			return err
		}
		id, err := result.LastInsertId()
		session = Session{ID: uint64(id), DisplayName: displayName}
		return err
	})
	if err != nil {
		return Session{}, nil, err
	}
	return session, issued, nil
}

func (s *Store) Abandon(id uint64) (bool, error) {
	result, err := s.db.Exec(`DELETE FROM sessions WHERE id = ? AND resumed = 0 AND `+unbanned, id)
	if err != nil {
		return false, err
	}
	deleted, err := result.RowsAffected()
	return deleted > 0, err
}

func (s *Store) Touch(ids []uint64, now time.Time) error {
	if len(ids) == 0 {
		return nil
	}
	return s.transact(func(tx *sql.Tx) error {
		for _, id := range ids {
			if _, err := tx.Exec(`UPDATE sessions SET last_used_at = ? WHERE id = ?`, now.Unix(), id); err != nil {
				return err
			}
		}
		return nil
	})
}

func (s *Store) Purge(now time.Time) (int64, error) {
	var purged int64
	err := s.transact(func(tx *sql.Tx) error {
		result, err := tx.Exec(`DELETE FROM sessions WHERE last_used_at < ? AND `+unbanned, s.cutoff(now))
		if err != nil {
			return err
		}
		purged, err = result.RowsAffected()
		return err
	})
	return purged, err
}

func (s *Store) cutoff(now time.Time) int64 {
	return now.Add(-s.lifetime).Unix()
}

func (s *Store) purge(tx *sql.Tx, now time.Time) error {
	_, err := tx.Exec(`DELETE FROM sessions WHERE last_used_at < ? AND `+unbanned, s.cutoff(now))
	return err
}

func (s *Store) Ban(ban Ban) (Ban, error) {
	var address any
	if ban.Address.IsValid() {
		address = ban.Address.String()
	}
	result, err := s.db.Exec(`INSERT INTO bans (session_id, display_name, address, created_at) VALUES (?, ?, ?, ?)`,
		ban.Session, ban.DisplayName, address, ban.CreatedAt.Unix())
	if err != nil {
		return Ban{}, err
	}
	id, err := result.LastInsertId()
	ban.ID = uint64(id)
	ban.CreatedAt = time.Unix(ban.CreatedAt.Unix(), 0)
	return ban, err
}

func (s *Store) Bans() ([]Ban, error) {
	rows, err := s.db.Query(`SELECT id, session_id, display_name, address, created_at FROM bans ORDER BY id`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	var bans []Ban
	for rows.Next() {
		var ban Ban
		var address sql.NullString
		var createdAt int64
		if err := rows.Scan(&ban.ID, &ban.Session, &ban.DisplayName, &address, &createdAt); err != nil {
			return nil, err
		}
		if address.Valid {
			if ban.Address, err = netip.ParsePrefix(address.String); err != nil {
				return nil, fmt.Errorf("ban %d: %w", ban.ID, err)
			}
		}
		ban.CreatedAt = time.Unix(createdAt, 0)
		bans = append(bans, ban)
	}
	return bans, rows.Err()
}

// Lift restarts the session's lifetime, which the ban held, so a player away for longer
// than it can still resume.
func (s *Store) Lift(id uint64, now time.Time) (bool, error) {
	lifted := false
	err := s.transact(func(tx *sql.Tx) error {
		var session uint64
		err := tx.QueryRow(`DELETE FROM bans WHERE id = ? RETURNING session_id`, id).Scan(&session)
		switch {
		case errors.Is(err, sql.ErrNoRows):
			return nil
		case err != nil:
			return err
		}
		lifted = true
		_, err = tx.Exec(`UPDATE sessions SET last_used_at = MAX(last_used_at, ?) WHERE id = ?`, now.Unix(), session)
		return err
	})
	return lifted, err
}

func (s *Store) AddressBanned(address netip.Prefix) (bool, error) {
	var banned bool
	err := s.db.QueryRow(`SELECT EXISTS (SELECT 1 FROM bans WHERE address = ?)`, address.String()).Scan(&banned)
	return banned, err
}

func (s *Store) transact(f func(*sql.Tx) error) error {
	tx, err := s.db.BeginTx(context.Background(), nil)
	if err != nil {
		return err
	}
	if err := f(tx); err != nil {
		tx.Rollback()
		return err
	}
	return tx.Commit()
}

func rename(tx *sql.Tx, id uint64, displayName string) error {
	if err := requireFree(tx, id, displayName); err != nil {
		return err
	}
	_, err := tx.Exec(`UPDATE sessions SET display_name = ?, display_name_key = ? WHERE id = ?`,
		displayName, validate.DisplayNameKey(displayName), id)
	return err
}

func requireFree(tx *sql.Tx, own uint64, displayName string) error {
	var holder uint64
	err := tx.QueryRow(`SELECT id FROM sessions WHERE display_name_key = ?`, validate.DisplayNameKey(displayName)).Scan(&holder)
	switch {
	case errors.Is(err, sql.ErrNoRows):
		return nil
	case err != nil:
		return err
	case holder != own:
		return ErrDisplayNameTaken
	}
	return nil
}
