package auth

import (
	"bytes"
	"crypto/sha256"
	"database/sql"
	"errors"
	"net/netip"
	"path/filepath"
	"testing"
	"time"
)

const month = 30 * 24 * time.Hour

var start = time.Date(2026, 10, 1, 12, 0, 0, 0, time.UTC)

func open(t *testing.T, path string) *Store {
	t.Helper()
	s, err := Open(path, month)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { s.Close() })
	return s
}

func issue(t *testing.T, s *Store, displayName string, now time.Time) (Session, []byte) {
	t.Helper()
	session, token, err := s.Authenticate(nil, displayName, now, nil)
	if err != nil {
		t.Fatalf("issue %q: %v", displayName, err)
	}
	if len(token) != 32 {
		t.Fatalf("issued a token of %d bytes", len(token))
	}
	return session, token
}

func TestIssueAndResume(t *testing.T) {
	s := open(t, "")
	lydia, token := issue(t, s, "Lydia", start)
	serana, other := issue(t, s, "Serana", start)
	if lydia.ID == 0 || lydia.ID == serana.ID || bytes.Equal(token, other) || lydia.DisplayName != "Lydia" {
		t.Fatalf("issued %+v and %+v", lydia, serana)
	}

	resumed, issued, err := s.Authenticate(token, "Lydia", start.Add(time.Hour), nil)
	if err != nil || resumed != lydia || issued != nil {
		t.Fatalf("resume: %+v %v %v", resumed, issued, err)
	}
}

func TestUnknownTokenGetsANewSession(t *testing.T) {
	s := open(t, "")
	lydia, _ := issue(t, s, "Lydia", start)
	session, issued, err := s.Authenticate(bytes.Repeat([]byte{7}, 32), "Serana", start, nil)
	if err != nil || issued == nil || session.ID == lydia.ID || session.DisplayName != "Serana" {
		t.Fatalf("got %+v %v %v", session, issued, err)
	}
}

func TestDisplayNamesAreUniqueWithoutCase(t *testing.T) {
	s := open(t, "")
	lydia, token := issue(t, s, "Lydia", start)
	if _, _, err := s.Authenticate(nil, "LYDIA", start, nil); !errors.Is(err, ErrDisplayNameTaken) {
		t.Fatalf("a second session took the name: %v", err)
	}

	_, seranaToken := issue(t, s, "Serana", start)
	if _, _, err := s.Authenticate(seranaToken, "lydia", start, nil); !errors.Is(err, ErrDisplayNameTaken) {
		t.Fatalf("a resumed session took the name: %v", err)
	}
	if session, _, err := s.Authenticate(seranaToken, "Serana", start, nil); err != nil || session.DisplayName != "Serana" {
		t.Fatalf("after a refused rename: %+v %v", session, err)
	}

	respelled, _, err := s.Authenticate(token, "LYDIA", start, nil)
	if err != nil || respelled.ID != lydia.ID || respelled.DisplayName != "LYDIA" {
		t.Fatalf("respelling: %+v %v", respelled, err)
	}
}

func TestResumingUnderAnotherDisplayNameRenames(t *testing.T) {
	s := open(t, "")
	lydia, token := issue(t, s, "Lydia", start)
	session, issued, err := s.Authenticate(token, "Jordis", start, nil)
	if err != nil || issued != nil || session != (Session{lydia.ID, "Jordis"}) {
		t.Fatalf("after rename: %+v %v %v", session, issued, err)
	}
	issue(t, s, "Lydia", start)
}

func TestSessionsExpireAfterTheirLifetimeUnused(t *testing.T) {
	s := open(t, "")
	lydia, token := issue(t, s, "Lydia", start)

	now := start
	for range 3 {
		now = now.Add(month - time.Hour)
		if session, issued, err := s.Authenticate(token, "Lydia", now, nil); err != nil || session.ID != lydia.ID || issued != nil {
			t.Fatalf("resume at %v: %+v %v %v", now, session, issued, err)
		}
	}

	now = now.Add(month - time.Hour)
	if err := s.Touch([]uint64{lydia.ID}, now); err != nil {
		t.Fatal(err)
	}
	now = now.Add(month - time.Hour)
	if session, _, err := s.Authenticate(token, "Lydia", now, nil); err != nil || session.ID != lydia.ID {
		t.Fatalf("resume after a touch: %+v %v", session, err)
	}

	now = now.Add(month + time.Hour)
	serana, issued, err := s.Authenticate(nil, "Lydia", now, nil)
	if err != nil || issued == nil || serana.ID == lydia.ID {
		t.Fatalf("the expired session kept its name: %+v %v", serana, err)
	}
	if _, _, err := s.Authenticate(token, "Lydia", now, nil); !errors.Is(err, ErrDisplayNameTaken) {
		t.Fatalf("the expired token resumed: %v", err)
	}
	renewed, issued, err := s.Authenticate(token, "Lydia the Second", now, nil)
	if err != nil || issued == nil || renewed.ID <= serana.ID {
		t.Fatalf("the expired token: %+v %v %v", renewed, issued, err)
	}
}

func TestPurge(t *testing.T) {
	s := open(t, "")
	issue(t, s, "Lydia", start)
	issue(t, s, "Serana", start.Add(2*time.Hour))
	if purged, err := s.Purge(start.Add(month + time.Hour)); err != nil || purged != 1 {
		t.Fatalf("purged %d: %v", purged, err)
	}
}

func TestSessionsSurviveARestart(t *testing.T) {
	path := filepath.Join(t.TempDir(), "sessions.db")
	first, err := Open(path, month)
	if err != nil {
		t.Fatal(err)
	}
	lydia, token, err := first.Authenticate(nil, "Lydia", start, nil)
	if err != nil {
		t.Fatal(err)
	}
	if err := first.Close(); err != nil {
		t.Fatal(err)
	}

	second := open(t, path)
	session, issued, err := second.Authenticate(token, "Lydia", start.Add(time.Hour), nil)
	if err != nil || session != lydia || issued != nil {
		t.Fatalf("after reopening: %+v %v %v", session, issued, err)
	}
	if _, err := second.Purge(start.Add(2 * month)); err != nil {
		t.Fatal(err)
	}
	next, _ := issue(t, second, "Serana", start.Add(2*month))
	if next.ID <= lydia.ID {
		t.Fatalf("ID %d was reused after %d", next.ID, lydia.ID)
	}
}

func TestAVersion1DatabaseIsUpgraded(t *testing.T) {
	path := filepath.Join(t.TempDir(), "sessions.db")
	db, err := sql.Open("sqlite", path)
	if err != nil {
		t.Fatal(err)
	}
	token := bytes.Repeat([]byte{7}, 32)
	hash := sha256.Sum256(token)
	if _, err := db.Exec(`
CREATE TABLE sessions (
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	token_hash BLOB NOT NULL UNIQUE,
	nickname TEXT NOT NULL,
	nickname_key TEXT NOT NULL UNIQUE,
	created_at INTEGER NOT NULL,
	last_used_at INTEGER NOT NULL,
	resumed INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX sessions_last_used_at ON sessions (last_used_at);
INSERT INTO sessions (token_hash, nickname, nickname_key, created_at, last_used_at, resumed)
	VALUES (?, 'Lydia', 'lydia', ?, ?, 1);
PRAGMA user_version = 1;
`, hash[:], start.Unix(), start.Unix()); err != nil {
		t.Fatal(err)
	}
	if err := db.Close(); err != nil {
		t.Fatal(err)
	}

	s := open(t, path)
	session, issued, err := s.Authenticate(token, "Lydia", start.Add(time.Hour), nil)
	if err != nil || session.ID != 1 || session.DisplayName != "Lydia" || issued != nil {
		t.Fatalf("after upgrading: %+v %v %v", session, issued, err)
	}
	if _, _, err := s.Authenticate(nil, "LYDIA", start.Add(time.Hour), nil); !errors.Is(err, ErrDisplayNameTaken) {
		t.Fatalf("the upgraded session's display name was given away: %v", err)
	}
	if _, err := s.Ban(Ban{Session: session.ID, DisplayName: "Lydia", CreatedAt: start}); err != nil {
		t.Fatalf("the upgraded database takes no bans: %v", err)
	}
	if err := s.Close(); err != nil {
		t.Fatal(err)
	}
	open(t, path)
}

func TestAVersion2DatabaseGetsBans(t *testing.T) {
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
PRAGMA user_version = 2;
`); err != nil {
		t.Fatal(err)
	}
	if err := db.Close(); err != nil {
		t.Fatal(err)
	}
	s := open(t, path)
	lydia, _ := issue(t, s, "Lydia", start)
	if _, err := s.Ban(Ban{Session: lydia.ID, DisplayName: "Lydia", CreatedAt: start}); err != nil {
		t.Fatal(err)
	}
}

func TestAbandonFreesTheDisplayNameOfASessionNeverResumed(t *testing.T) {
	s := open(t, "")
	lydia, token := issue(t, s, "Lydia", start)
	if abandoned, err := s.Abandon(lydia.ID); err != nil || !abandoned {
		t.Fatalf("abandon: %v %v", abandoned, err)
	}
	again, issued, err := s.Authenticate(token, "Lydia", start, nil)
	if err != nil || issued == nil || again.ID <= lydia.ID {
		t.Fatalf("after abandon: %+v %v %v", again, issued, err)
	}

	if _, _, err := s.Authenticate(issued, "Lydia", start, nil); err != nil {
		t.Fatal(err)
	}
	if abandoned, err := s.Abandon(again.ID); err != nil || abandoned {
		t.Fatalf("abandoned a resumed session: %v %v", abandoned, err)
	}
	if session, _, err := s.Authenticate(issued, "Lydia", start, nil); err != nil || session.ID != again.ID {
		t.Fatalf("after a refused abandon: %+v %v", session, err)
	}
}

func TestMayIssueIsAskedOnlyForANewSession(t *testing.T) {
	s := open(t, "")
	_, token := issue(t, s, "Lydia", start)
	asked := 0
	refuse := func() bool { asked++; return false }
	if _, _, err := s.Authenticate(token, "Lydia", start, refuse); err != nil || asked != 0 {
		t.Fatalf("resume: %v, asked %d", err, asked)
	}
	if _, _, err := s.Authenticate(nil, "lydia", start, refuse); !errors.Is(err, ErrDisplayNameTaken) || asked != 0 {
		t.Fatalf("taken: %v, asked %d", err, asked)
	}
	if _, _, err := s.Authenticate(nil, "Serana", start, refuse); !errors.Is(err, ErrIssueRefused) || asked != 1 {
		t.Fatalf("new: %v, asked %d", err, asked)
	}
	issue(t, s, "Serana", start)
}

func TestBannedSessionIsRefusedAndKept(t *testing.T) {
	s := open(t, "")
	lydia, token := issue(t, s, "Lydia", start)
	ban, err := s.Ban(Ban{Session: lydia.ID, DisplayName: "Lydia", CreatedAt: start})
	if err != nil || ban.ID == 0 {
		t.Fatalf("ban %+v %v", ban, err)
	}
	if _, _, err := s.Authenticate(token, "Lydia", start, nil); !errors.Is(err, ErrBanned) {
		t.Fatalf("a banned session resumed: %v", err)
	}
	if _, _, err := s.Authenticate(token, "Jordis", start, nil); !errors.Is(err, ErrBanned) {
		t.Fatalf("a banned session renamed: %v", err)
	}

	later := start.Add(2 * month)
	if purged, err := s.Purge(later); purged != 0 || err != nil {
		t.Fatalf("purged %d %v", purged, err)
	}
	if abandoned, err := s.Abandon(lydia.ID); abandoned || err != nil {
		t.Fatalf("abandoned %v %v", abandoned, err)
	}
	if _, _, err := s.Authenticate(nil, "lydia", later, nil); !errors.Is(err, ErrDisplayNameTaken) {
		t.Fatalf("the banned session's display name was given away: %v", err)
	}

	if lifted, err := s.Lift(ban.ID, later); !lifted || err != nil {
		t.Fatalf("lift: %v %v", lifted, err)
	}
	if lifted, _ := s.Lift(ban.ID, later); lifted {
		t.Fatal("lifted a ban twice")
	}
	// Lifting restarts the lifetime the ban held.
	if purged, _ := s.Purge(later.Add(time.Hour)); purged != 0 {
		t.Fatal("the session expired as its ban was lifted")
	}
	if resumed, _, err := s.Authenticate(token, "Lydia", later, nil); err != nil || resumed.ID != lydia.ID {
		t.Fatalf("after lifting: %+v %v", resumed, err)
	}
}

func TestAddressBans(t *testing.T) {
	s := open(t, "")
	home := netip.MustParsePrefix("2001:db8:1:2::/64")
	first, _ := s.Ban(Ban{Session: 1, DisplayName: "Lydia", CreatedAt: start})
	second, err := s.Ban(Ban{Session: 2, DisplayName: "Serana", Address: home, CreatedAt: start.Add(time.Minute)})
	if err != nil {
		t.Fatal(err)
	}
	for prefix, want := range map[netip.Prefix]bool{home: true, netip.MustParsePrefix("2001:db8:1:3::/64"): false} {
		if banned, err := s.AddressBanned(prefix); banned != want || err != nil {
			t.Errorf("%v: banned %v %v", prefix, banned, err)
		}
	}
	bans, err := s.Bans()
	if err != nil || len(bans) != 2 || bans[0] != first || bans[1] != second || bans[1].Address != home ||
		!bans[1].CreatedAt.Equal(start.Add(time.Minute)) {
		t.Fatalf("bans %+v %v", bans, err)
	}
	s.Lift(second.ID, start)
	if banned, _ := s.AddressBanned(home); banned {
		t.Fatal("the address stayed banned")
	}
}
