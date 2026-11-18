package metrics

import (
	"context"
	"database/sql"
	"fmt"
	"time"

	_ "modernc.org/sqlite"
)

const schemaVersion = 1

const schema = `
CREATE TABLE meta (
	key TEXT PRIMARY KEY,
	value INTEGER NOT NULL
);
CREATE TABLE connections (
	id INTEGER PRIMARY KEY,
	session_id INTEGER NOT NULL,
	display_name TEXT NOT NULL,
	started_at INTEGER NOT NULL,
	-- NULL while connected.
	ended_at INTEGER,
	end_reason TEXT
);
CREATE INDEX connections_started_at ON connections (started_at);
CREATE INDEX connections_ended_at ON connections (ended_at);
CREATE TABLE players (
	session_id INTEGER PRIMARY KEY,
	first_seen_at INTEGER NOT NULL
);
CREATE INDEX players_first_seen_at ON players (first_seen_at);
CREATE TABLE samples (
	at INTEGER PRIMARY KEY,
	seconds REAL NOT NULL,
	players INTEGER NOT NULL,
	in_world INTEGER NOT NULL,
	bytes_in INTEGER NOT NULL,
	bytes_out INTEGER NOT NULL,
	busy_ms REAL NOT NULL,
	worst_ms REAL NOT NULL
);
CREATE TABLE hourly (
	at INTEGER PRIMARY KEY,
	samples INTEGER NOT NULL,
	seconds REAL NOT NULL,
	players_sum INTEGER NOT NULL,
	players_max INTEGER NOT NULL,
	in_world_sum INTEGER NOT NULL,
	bytes_in INTEGER NOT NULL,
	bytes_out INTEGER NOT NULL,
	out_rate_max REAL NOT NULL,
	busy_ms REAL NOT NULL,
	worst_ms REAL NOT NULL
);
CREATE TABLE refusals (
	at INTEGER NOT NULL,
	reason TEXT NOT NULL,
	count INTEGER NOT NULL,
	PRIMARY KEY (at, reason)
);
`

// Minute samples outlive this only as hourly rows.
const sampleRetention = 14 * 24 * time.Hour

type Store struct {
	db        *sql.DB
	retention time.Duration
	fresh     bool
	createdAt time.Time
}

func Open(path string, retention time.Duration) (*Store, error) {
	if path == "" {
		path = ":memory:"
	}
	db, err := sql.Open("sqlite", path+"?_pragma=busy_timeout(5000)&_pragma=journal_mode(WAL)&_pragma=synchronous(NORMAL)")
	if err != nil {
		return nil, err
	}
	// A second connection to an in-memory database would open another one.
	db.SetMaxOpenConns(1)
	s := &Store{db: db, retention: retention}
	if err := s.migrate(time.Now()); err != nil {
		db.Close()
		return nil, err
	}
	return s, nil
}

func (s *Store) Close() error {
	return s.db.Close()
}

// Fresh reports whether Open created the database.
func (s *Store) Fresh() bool {
	return s.fresh
}

func (s *Store) migrate(now time.Time) error {
	var version int
	if err := s.db.QueryRow(`PRAGMA user_version`).Scan(&version); err != nil {
		return err
	}
	switch version {
	case schemaVersion:
	case 0:
		err := s.transact(func(tx *sql.Tx) error {
			if _, err := tx.Exec(schema); err != nil {
				return err
			}
			if _, err := tx.Exec(`INSERT INTO meta (key, value) VALUES ('created_at', ?)`, now.UnixMilli()); err != nil {
				return err
			}
			_, err := tx.Exec(fmt.Sprintf(`PRAGMA user_version = %d`, schemaVersion))
			return err
		})
		if err != nil {
			return err
		}
		s.fresh = true
	default:
		return fmt.Errorf("metrics database has schema %d, this server knows %d", version, schemaVersion)
	}
	var created int64
	if err := s.db.QueryRow(`SELECT value FROM meta WHERE key = 'created_at'`).Scan(&created); err != nil {
		return err
	}
	s.createdAt = time.UnixMilli(created)
	return nil
}

// AddPlayers records sessions first seen before the database existed.
func (s *Store) AddPlayers(firstSeen map[uint64]time.Time) error {
	return s.transact(func(tx *sql.Tx) error {
		for session, at := range firstSeen {
			if _, err := tx.Exec(`INSERT OR IGNORE INTO players (session_id, first_seen_at) VALUES (?, ?)`,
				session, at.UnixMilli()); err != nil {
				return err
			}
		}
		return nil
	})
}

func (s *Store) lastConnectionID() (uint64, error) {
	var id uint64
	err := s.db.QueryRow(`SELECT COALESCE(MAX(id), 0) FROM connections`).Scan(&id)
	return id, err
}

// closeAbandoned ends the connections a server that stopped without closing them left open, at
// the end of its last sample.
func (s *Store) closeAbandoned() (int64, error) {
	result, err := s.db.Exec(`UPDATE connections
		SET ended_at = MAX(started_at, COALESCE((SELECT MAX(at + CAST(seconds * 1000 AS INTEGER)) FROM samples), started_at)),
			end_reason = ?
		WHERE ended_at IS NULL`, ReasonServerLost)
	if err != nil {
		return 0, err
	}
	return result.RowsAffected()
}

func (s *Store) closeOpen(at time.Time, reason string) error {
	_, err := s.db.Exec(`UPDATE connections SET ended_at = MAX(started_at, ?), end_reason = ? WHERE ended_at IS NULL`,
		at.UnixMilli(), reason)
	return err
}

func (s *Store) writeEvents(events []event) error {
	return s.transact(func(tx *sql.Tx) error {
		for _, e := range events {
			var err error
			if e.reason == "" {
				if _, err = tx.Exec(`INSERT INTO connections (id, session_id, display_name, started_at) VALUES (?, ?, ?, ?)`,
					e.id, e.session, e.displayName, e.at.UnixMilli()); err != nil {
					return err
				}
				_, err = tx.Exec(`INSERT OR IGNORE INTO players (session_id, first_seen_at) VALUES (?, ?)`,
					e.session, e.at.UnixMilli())
			} else {
				_, err = tx.Exec(`UPDATE connections SET ended_at = MAX(started_at, ?), end_reason = ? WHERE id = ?`,
					e.at.UnixMilli(), e.reason, e.id)
			}
			if err != nil {
				return err
			}
		}
		return nil
	})
}

type sample struct {
	at                time.Time
	seconds           float64
	players, inWorld  int
	bytesIn, bytesOut uint64
	busyMs, worstMs   float64
}

// writeSample stores a minute and folds it into its hour, and adds the refusals to their hour.
func (s *Store) writeSample(v sample, refusals map[string]int) error {
	hour := v.at.Truncate(time.Hour).UnixMilli()
	return s.transact(func(tx *sql.Tx) error {
		if _, err := tx.Exec(`INSERT OR REPLACE INTO samples (at, seconds, players, in_world, bytes_in, bytes_out, busy_ms, worst_ms)
			VALUES (?, ?, ?, ?, ?, ?, ?, ?)`, v.at.UnixMilli(), v.seconds, v.players, v.inWorld, v.bytesIn, v.bytesOut,
			v.busyMs, v.worstMs); err != nil {
			return err
		}
		if _, err := tx.Exec(`INSERT INTO hourly (at, samples, seconds, players_sum, players_max, in_world_sum, bytes_in,
				bytes_out, out_rate_max, busy_ms, worst_ms)
			SELECT ?, COUNT(*), SUM(seconds), SUM(players), MAX(players), SUM(in_world), SUM(bytes_in), SUM(bytes_out),
				MAX(bytes_out / seconds), SUM(busy_ms), MAX(worst_ms)
			FROM samples WHERE at >= ? AND at < ?
			ON CONFLICT (at) DO UPDATE SET samples = excluded.samples, seconds = excluded.seconds,
				players_sum = excluded.players_sum, players_max = excluded.players_max,
				in_world_sum = excluded.in_world_sum, bytes_in = excluded.bytes_in, bytes_out = excluded.bytes_out,
				out_rate_max = excluded.out_rate_max, busy_ms = excluded.busy_ms, worst_ms = excluded.worst_ms`,
			hour, hour, hour+time.Hour.Milliseconds()); err != nil {
			return err
		}
		return insertRefusals(tx, hour, refusals)
	})
}

func (s *Store) addRefusals(at time.Time, refusals map[string]int) error {
	if len(refusals) == 0 {
		return nil
	}
	return s.transact(func(tx *sql.Tx) error {
		return insertRefusals(tx, at.Truncate(time.Hour).UnixMilli(), refusals)
	})
}

func insertRefusals(tx *sql.Tx, hour int64, refusals map[string]int) error {
	for reason, count := range refusals {
		if _, err := tx.Exec(`INSERT INTO refusals (at, reason, count) VALUES (?, ?, ?)
			ON CONFLICT (at, reason) DO UPDATE SET count = count + excluded.count`, hour, reason, count); err != nil {
			return err
		}
	}
	return nil
}

func (s *Store) purge(now time.Time) error {
	cutoff := now.Add(-s.retention).UnixMilli()
	return s.transact(func(tx *sql.Tx) error {
		statements := []struct {
			query  string
			cutoff int64
		}{
			{`DELETE FROM samples WHERE at < ?`, max(cutoff, now.Add(-sampleRetention).UnixMilli())},
			{`DELETE FROM hourly WHERE at < ?`, cutoff},
			{`DELETE FROM refusals WHERE at < ?`, cutoff},
			{`DELETE FROM connections WHERE ended_at < ?`, cutoff},
		}
		for _, statement := range statements {
			if _, err := tx.Exec(statement.query, statement.cutoff); err != nil {
				return err
			}
		}
		return nil
	})
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
