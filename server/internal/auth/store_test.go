package auth

import (
	"bytes"
	"errors"
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

func issue(t *testing.T, s *Store, nickname string, now time.Time) (Session, []byte) {
	t.Helper()
	session, token, err := s.Authenticate(nil, nickname, now)
	if err != nil {
		t.Fatalf("issue %q: %v", nickname, err)
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
	if lydia.ID == 0 || lydia.ID == serana.ID || bytes.Equal(token, other) || lydia.Nickname != "Lydia" {
		t.Fatalf("issued %+v and %+v", lydia, serana)
	}

	resumed, issued, err := s.Authenticate(token, "Lydia", start.Add(time.Hour))
	if err != nil || resumed != lydia || issued != nil {
		t.Fatalf("resume: %+v %v %v", resumed, issued, err)
	}
}

func TestUnknownTokenGetsANewSession(t *testing.T) {
	s := open(t, "")
	lydia, _ := issue(t, s, "Lydia", start)
	session, issued, err := s.Authenticate(bytes.Repeat([]byte{7}, 32), "Serana", start)
	if err != nil || issued == nil || session.ID == lydia.ID || session.Nickname != "Serana" {
		t.Fatalf("got %+v %v %v", session, issued, err)
	}
}

func TestNicknamesAreUniqueWithoutCase(t *testing.T) {
	s := open(t, "")
	lydia, token := issue(t, s, "Lydia", start)
	if _, _, err := s.Authenticate(nil, "LYDIA", start); !errors.Is(err, ErrNicknameTaken) {
		t.Fatalf("a second session took the name: %v", err)
	}

	_, seranaToken := issue(t, s, "Serana", start)
	if _, _, err := s.Authenticate(seranaToken, "lydia", start); !errors.Is(err, ErrNicknameTaken) {
		t.Fatalf("a resumed session took the name: %v", err)
	}
	// The refused rename left the session as it was.
	if session, _, err := s.Authenticate(seranaToken, "Serana", start); err != nil || session.Nickname != "Serana" {
		t.Fatalf("after a refused rename: %+v %v", session, err)
	}

	// Its own name in another spelling is a rename, not a conflict.
	respelled, _, err := s.Authenticate(token, "LYDIA", start)
	if err != nil || respelled.ID != lydia.ID || respelled.Nickname != "LYDIA" {
		t.Fatalf("respelling: %+v %v", respelled, err)
	}
}

func TestRename(t *testing.T) {
	s := open(t, "")
	lydia, token := issue(t, s, "Lydia", start)
	issue(t, s, "Serana", start)
	if err := s.Rename(lydia.ID, "serana", start); !errors.Is(err, ErrNicknameTaken) {
		t.Fatalf("renamed to a held name: %v", err)
	}
	if err := s.Rename(lydia.ID, "Jordis", start); err != nil {
		t.Fatal(err)
	}
	if session, _, err := s.Authenticate(token, "Jordis", start); err != nil || session != (Session{lydia.ID, "Jordis"}) {
		t.Fatalf("after rename: %+v %v", session, err)
	}
	// The old name is free at once.
	issue(t, s, "Lydia", start)
	if err := s.Rename(999, "Aela", start); !errors.Is(err, ErrUnknownSession) {
		t.Fatalf("renamed a missing session: %v", err)
	}
}

func TestSessionsExpireAfterTheirLifetimeUnused(t *testing.T) {
	s := open(t, "")
	lydia, token := issue(t, s, "Lydia", start)

	// Each use within the lifetime extends it.
	now := start
	for range 3 {
		now = now.Add(month - time.Hour)
		if session, issued, err := s.Authenticate(token, "Lydia", now); err != nil || session.ID != lydia.ID || issued != nil {
			t.Fatalf("resume at %v: %+v %v %v", now, session, issued, err)
		}
	}

	// Being connected counts as use.
	now = now.Add(month - time.Hour)
	if err := s.Touch([]uint64{lydia.ID}, now); err != nil {
		t.Fatal(err)
	}
	now = now.Add(month - time.Hour)
	if session, _, err := s.Authenticate(token, "Lydia", now); err != nil || session.ID != lydia.ID {
		t.Fatalf("resume after a touch: %+v %v", session, err)
	}

	// Past the lifetime the token is worth nothing and the name is free.
	now = now.Add(month + time.Hour)
	serana, issued, err := s.Authenticate(nil, "Lydia", now)
	if err != nil || issued == nil || serana.ID == lydia.ID {
		t.Fatalf("the expired session kept its name: %+v %v", serana, err)
	}
	if _, _, err := s.Authenticate(token, "Lydia", now); !errors.Is(err, ErrNicknameTaken) {
		t.Fatalf("the expired token resumed: %v", err)
	}
	renewed, issued, err := s.Authenticate(token, "Lydia the Second", now)
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
	lydia, token, err := first.Authenticate(nil, "Lydia", start)
	if err != nil {
		t.Fatal(err)
	}
	if err := first.Close(); err != nil {
		t.Fatal(err)
	}

	second := open(t, path)
	session, issued, err := second.Authenticate(token, "Lydia", start.Add(time.Hour))
	if err != nil || session != lydia || issued != nil {
		t.Fatalf("after reopening: %+v %v %v", session, issued, err)
	}
	// IDs are not reused, so a later session cannot be taken for an earlier one.
	if err := second.Rename(lydia.ID, "Jordis", start); err != nil {
		t.Fatal(err)
	}
	if _, err := second.Purge(start.Add(2 * month)); err != nil {
		t.Fatal(err)
	}
	next, _ := issue(t, second, "Serana", start.Add(2*month))
	if next.ID <= lydia.ID {
		t.Fatalf("ID %d was reused after %d", next.ID, lydia.ID)
	}
}

func TestAbandonFreesTheNicknameOfASessionNeverResumed(t *testing.T) {
	s := open(t, "")
	lydia, token := issue(t, s, "Lydia", start)
	if abandoned, err := s.Abandon(lydia.ID); err != nil || !abandoned {
		t.Fatalf("abandon: %v %v", abandoned, err)
	}
	again, issued, err := s.Authenticate(token, "Lydia", start)
	if err != nil || issued == nil || again.ID <= lydia.ID {
		t.Fatalf("after abandon: %+v %v %v", again, issued, err)
	}

	// A token that came back is held by its client, in the same second too.
	if _, _, err := s.Authenticate(issued, "Lydia", start); err != nil {
		t.Fatal(err)
	}
	if abandoned, err := s.Abandon(again.ID); err != nil || abandoned {
		t.Fatalf("abandoned a resumed session: %v %v", abandoned, err)
	}
	if session, _, err := s.Authenticate(issued, "Lydia", start); err != nil || session.ID != again.ID {
		t.Fatalf("after a refused abandon: %+v %v", session, err)
	}
}
