package admission

import (
	"net/http"
	"net/netip"
	"testing"
	"time"
)

func TestPendingHellosAreCapped(t *testing.T) {
	g := NewGate(Limits{MaxPendingHellos: 2})
	if !g.BeginHello() || !g.BeginHello() {
		t.Fatal("refused below the cap")
	}
	if g.BeginHello() {
		t.Fatal("allowed beyond the cap")
	}
	g.EndHello()
	if !g.BeginHello() {
		t.Fatal("a finished hello did not free its place")
	}
}

func TestPlayersAreCapped(t *testing.T) {
	g := NewGate(Limits{MaxPlayers: 2})
	if !g.Admit(1) || !g.Admit(2) {
		t.Fatal("refused below the cap")
	}
	if !g.Full() || g.Admit(3) {
		t.Fatal("admitted beyond the cap")
	}
	if !g.Admit(2) {
		t.Fatal("a connected session could not connect again")
	}
	g.Leave(2)
	if !g.Full() {
		t.Fatal("the replaced connection freed the session's place")
	}
	g.Leave(2)
	if g.Full() || !g.Admit(3) {
		t.Fatal("a session that left kept its place")
	}
	if NewGate(Limits{}).Full() {
		t.Fatal("zero is a cap")
	}
}

func TestNewSessionsPerAddress(t *testing.T) {
	g := NewGate(Limits{NewSessionsPerAddressPerHour: 2})
	start := time.Unix(0, 0)
	first := netip.MustParseAddr("203.0.113.7")
	for range 2 {
		if !g.MayIssue(first, start) {
			t.Fatal("refused within the allowance")
		}
	}
	if g.MayIssue(first, start) {
		t.Fatal("allowed beyond the allowance")
	}
	if !g.MayIssue(netip.MustParseAddr("::ffff:203.0.113.8"), start) {
		t.Fatal("another address shares the allowance")
	}
	if g.MayIssue(first, start.Add(29*time.Minute)) || !g.MayIssue(first, start.Add(31*time.Minute)) {
		t.Fatal("the allowance does not refill at its hourly rate")
	}

	subnet := netip.MustParseAddr("2001:db8:1:2::1")
	g.MayIssue(subnet, start)
	g.MayIssue(netip.MustParseAddr("2001:db8:1:2:ffff::9"), start)
	if g.MayIssue(subnet, start) {
		t.Fatal("an IPv6 /64 has more than one allowance")
	}
	if !g.MayIssue(netip.MustParseAddr("2001:db8:1:3::1"), start) {
		t.Fatal("another /64 shares the allowance")
	}

	g.Prune(start.Add(2 * time.Hour))
	if len(g.issued) != 0 {
		t.Fatalf("refilled allowances kept: %d", len(g.issued))
	}
	if !NewGate(Limits{}).MayIssue(first, start) {
		t.Fatal("zero is a limit")
	}
}

func TestClientAddress(t *testing.T) {
	trusted := []netip.Prefix{netip.MustParsePrefix("127.0.0.1/32"), netip.MustParsePrefix("10.0.0.0/8")}
	for _, tc := range []struct {
		remote    string
		forwarded []string
		want      string
	}{
		{"198.51.100.4:5000", []string{"203.0.113.9"}, "198.51.100.4"},
		{"127.0.0.1:5000", nil, "127.0.0.1"},
		{"127.0.0.1:5000", []string{"203.0.113.9"}, "203.0.113.9"},
		{"127.0.0.1:5000", []string{"1.2.3.4, 203.0.113.9"}, "203.0.113.9"},
		{"127.0.0.1:5000", []string{"1.2.3.4", "203.0.113.9, 10.1.1.1"}, "203.0.113.9"},
		{"127.0.0.1:5000", []string{"10.1.1.1"}, "10.1.1.1"},
		{"127.0.0.1:5000", []string{"203.0.113.9, garbage"}, "127.0.0.1"},
		{"[::ffff:127.0.0.1]:5000", []string{"2001:db8::1"}, "2001:db8::1"},
	} {
		r := &http.Request{RemoteAddr: tc.remote, Header: http.Header{}}
		for _, value := range tc.forwarded {
			r.Header.Add("X-Forwarded-For", value)
		}
		if got := ClientAddress(r, trusted); got.String() != tc.want {
			t.Errorf("%s %v: got %s, want %s", tc.remote, tc.forwarded, got, tc.want)
		}
	}
}
