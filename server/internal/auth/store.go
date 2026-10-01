// Package auth keeps player sessions. A session is a player's identity on
// this server: a public ID, a secret token only its client holds, and a
// nickname no other live session has. It lives for as long as it keeps being
// used.
package auth

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"database/sql"
	"errors"
	"fmt"
	"time"

	_ "modernc.org/sqlite"

	"skyrim-mp/server/internal/validate"
)

var (
	ErrNicknameTaken  = errors.New("nickname taken")
	ErrUnknownSession = errors.New("unknown session")
)

const schemaVersion = 1

const schema = `
CREATE TABLE sessions (
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	token_hash BLOB NOT NULL UNIQUE,
	nickname TEXT NOT NULL,
	nickname_key TEXT NOT NULL UNIQUE,
	created_at INTEGER NOT NULL,
	last_used_at INTEGER NOT NULL,
	-- The token came back in a hello, so its client holds it.
	resumed INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX sessions_last_used_at ON sessions (last_used_at);
`

type Session struct {
	// Never reused, also after the session is gone.
	ID       uint64
	Nickname string
}

// Store is safe for concurrent use; its one connection serializes every
// transaction, which the nickname checks rely on.
type Store struct {
	db       *sql.DB
	lifetime time.Duration
}

// Open opens the database at path, creating it when missing. An empty path
// keeps the sessions in memory, for tests and load tests. A session unused
// for lifetime is gone.
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
	switch {
	case version == schemaVersion:
		return nil
	case version != 0:
		return fmt.Errorf("session database has schema %d, this server knows %d", version, schemaVersion)
	}
	return s.transact(func(tx *sql.Tx) error {
		if _, err := tx.Exec(schema); err != nil {
			return err
		}
		_, err := tx.Exec(fmt.Sprintf(`PRAGMA user_version = %d`, schemaVersion))
		return err
	})
}

// Authenticate resumes the live session the token belongs to, renaming it
// when nickname differs from its own, or issues a new session when the token
// is empty or matches none. Only a new session returns its token, which is
// not stored and cannot be recovered. ErrNicknameTaken leaves everything as
// it was.
func (s *Store) Authenticate(token []byte, nickname string, now time.Time) (Session, []byte, error) {
	var session Session
	var issued []byte
	err := s.transact(func(tx *sql.Tx) error {
		if err := s.purge(tx, now); err != nil {
			return err
		}
		if len(token) != 0 {
			hash := sha256.Sum256(token)
			var current string
			err := tx.QueryRow(`SELECT id, nickname FROM sessions WHERE token_hash = ?`, hash[:]).Scan(&session.ID, &current)
			switch {
			case err == nil:
				session.Nickname = nickname
				if current != nickname {
					if err := rename(tx, session.ID, nickname); err != nil {
						return err
					}
				}
				_, err = tx.Exec(`UPDATE sessions SET last_used_at = ?, resumed = 1 WHERE id = ?`, now.Unix(), session.ID)
				return err
			case !errors.Is(err, sql.ErrNoRows):
				return err
			}
		}
		if err := requireFree(tx, 0, nickname); err != nil {
			return err
		}
		issued = make([]byte, validate.SessionTokenBytes)
		if _, err := rand.Read(issued); err != nil {
			return err
		}
		hash := sha256.Sum256(issued)
		result, err := tx.Exec(
			`INSERT INTO sessions (token_hash, nickname, nickname_key, created_at, last_used_at) VALUES (?, ?, ?, ?, ?)`,
			hash[:], nickname, validate.NicknameKey(nickname), now.Unix(), now.Unix())
		if err != nil {
			return err
		}
		id, err := result.LastInsertId()
		session = Session{ID: uint64(id), Nickname: nickname}
		return err
	})
	if err != nil {
		return Session{}, nil, err
	}
	return session, issued, nil
}

// Rename gives the session another nickname, or another spelling of its own.
func (s *Store) Rename(id uint64, nickname string, now time.Time) error {
	return s.transact(func(tx *sql.Tx) error {
		if err := s.purge(tx, now); err != nil {
			return err
		}
		return rename(tx, id, nickname)
	})
}

// Abandon deletes a session whose token never came back, which frees its
// nickname, and reports whether it did. A session that was resumed stays: its
// client holds the token.
func (s *Store) Abandon(id uint64) (bool, error) {
	result, err := s.db.Exec(`DELETE FROM sessions WHERE id = ? AND resumed = 0`, id)
	if err != nil {
		return false, err
	}
	deleted, err := result.RowsAffected()
	return deleted > 0, err
}

// Touch marks the sessions used at now, so the lifetime of a session counts
// from when its player was last connected.
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

// Purge deletes the sessions that expired, which frees their nicknames, and
// returns how many. Authenticate and Rename do the same before they compare
// nicknames, so this only keeps the database small.
func (s *Store) Purge(now time.Time) (int64, error) {
	var purged int64
	err := s.transact(func(tx *sql.Tx) error {
		result, err := tx.Exec(`DELETE FROM sessions WHERE last_used_at < ?`, s.cutoff(now))
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
	_, err := tx.Exec(`DELETE FROM sessions WHERE last_used_at < ?`, s.cutoff(now))
	return err
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

func rename(tx *sql.Tx, id uint64, nickname string) error {
	if err := requireFree(tx, id, nickname); err != nil {
		return err
	}
	result, err := tx.Exec(`UPDATE sessions SET nickname = ?, nickname_key = ? WHERE id = ?`,
		nickname, validate.NicknameKey(nickname), id)
	if err != nil {
		return err
	}
	changed, err := result.RowsAffected()
	if err == nil && changed == 0 {
		return ErrUnknownSession
	}
	return err
}

// requireFree fails when a session other than own holds the nickname.
func requireFree(tx *sql.Tx, own uint64, nickname string) error {
	var holder uint64
	err := tx.QueryRow(`SELECT id FROM sessions WHERE nickname_key = ?`, validate.NicknameKey(nickname)).Scan(&holder)
	switch {
	case errors.Is(err, sql.ErrNoRows):
		return nil
	case err != nil:
		return err
	case holder != own:
		return ErrNicknameTaken
	}
	return nil
}
