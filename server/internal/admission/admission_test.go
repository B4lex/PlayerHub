package admission

import (
	"net/http"
	"net/netip"
	"testing"
	"time"
)

func TestPendingHellosAreCapped(t *testing.T) {
	g := NewGate(Limits{MaxPendingHellos: 2})
	first := netip.MustParseAddr("203.0.113.1")
	second := netip.MustParseAddr("203.0.113.2")
	if g.BeginHello(first) != nil || g.BeginHello(second) != nil {
		t.Fatal("refused below the cap")
	}
	if g.BeginHello(netip.MustParseAddr("203.0.113.3")) != ErrPendingHellos {
		t.Fatal("allowed beyond the cap")
	}
	g.EndHello(first)
	if g.BeginHello(first) != nil {
		t.Fatal("a finished hello did not free its place")
	}
}

func TestPendingHellosPerAddressAreCapped(t *testing.T) {
	g := NewGate(Limits{MaxPendingHellos: 8, MaxPendingHellosPerAddress: 2})
	flooding := netip.MustParseAddr("203.0.113.1")
	for range 2 {
		if g.BeginHello(flooding) != nil {
			t.Fatal("refused below the cap")
		}
	}
	if g.BeginHello(netip.MustParseAddr("::ffff:203.0.113.1")) != ErrAddressPendingHellos {
		t.Fatal("allowed beyond the address's cap")
	}
	if g.BeginHello(netip.MustParseAddr("203.0.113.2")) != nil {
		t.Fatal("another address shares the cap")
	}
	g.BeginHello(netip.MustParseAddr("2001:db8:1:2::1"))
	g.BeginHello(netip.MustParseAddr("2001:db8:1:2::2"))
	if g.BeginHello(netip.MustParseAddr("2001:db8:1:2:ffff::9")) != ErrAddressPendingHellos {
		t.Fatal("an IPv6 /64 has more than one cap")
	}
	g.EndHello(flooding)
	if g.BeginHello(flooding) != nil {
		t.Fatal("a finished hello did not free its address's place")
	}
	g.EndHello(flooding)
	g.EndHello(flooding)
	if _, kept := g.pendingByAddress[addressKey(flooding)]; kept {
		t.Fatal("an address without pending hellos is kept")
	}
	unlimited := NewGate(Limits{MaxPendingHellos: 8})
	for range 8 {
		if unlimited.BeginHello(flooding) != nil {
			t.Fatal("zero is a cap")
		}
	}
}

func TestPlayersAreCapped(t *testing.T) {
	g := NewGate(Limits{MaxPlayers: 2})
	here := netip.MustParseAddr("203.0.113.7")
	if g.Admit(1, here) != nil || g.Admit(2, here) != nil {
		t.Fatal("refused below the cap")
	}
	if g.CheckNew(here) != ErrServerFull || g.Admit(3, here) != ErrServerFull {
		t.Fatal("admitted beyond the cap")
	}
	if g.Admit(2, here) != nil {
		t.Fatal("a connected session could not connect again")
	}
	g.Leave(2, here)
	if g.CheckNew(here) != ErrServerFull {
		t.Fatal("the replaced connection freed the session's place")
	}
	g.Leave(2, here)
	if g.CheckNew(here) != nil || g.Admit(3, here) != nil {
		t.Fatal("a session that left kept its place")
	}
	if NewGate(Limits{}).CheckNew(here) != nil {
		t.Fatal("zero is a cap")
	}
}

func TestPlayersPerAddressAreCapped(t *testing.T) {
	g := NewGate(Limits{MaxPlayers: 8, MaxPlayersPerAddress: 2})
	home := netip.MustParseAddr("203.0.113.7")
	other := netip.MustParseAddr("203.0.113.8")
	phone := netip.MustParseAddr("2001:db8:1:2::1")
	if g.Admit(1, home) != nil || g.Admit(2, home) != nil {
		t.Fatal("refused below the cap")
	}
	if g.CheckNew(home) != ErrAddressFull || g.Admit(3, home) != ErrAddressFull {
		t.Fatal("admitted beyond the address's cap")
	}
	if g.Admit(3, other) != nil {
		t.Fatal("another address shares the cap")
	}
	if g.Admit(2, home) != nil {
		t.Fatal("a session could not connect again from its address")
	}
	if g.Admit(3, home) != ErrAddressFull {
		t.Fatal("a session connected elsewhere moved in beyond the cap")
	}
	g.Leave(2, home)
	if g.CheckNew(home) != ErrAddressFull {
		t.Fatal("the replaced connection freed the session's place")
	}
	g.Leave(2, home)
	if g.CheckNew(home) != nil || g.Admit(4, home) != nil {
		t.Fatal("a session that left kept its place")
	}

	if g.Admit(5, phone) != nil || g.Admit(6, netip.MustParseAddr("2001:db8:1:2::ffff")) != nil {
		t.Fatal("refused below the cap")
	}
	if g.CheckNew(netip.MustParseAddr("2001:db8:1:2:abcd::1")) != ErrAddressFull {
		t.Fatal("an IPv6 address escaped its /64's cap")
	}
	if g.CheckNew(netip.MustParseAddr("2001:db8:1:3::1")) != nil {
		t.Fatal("another /64 shares the cap")
	}
	if NewGate(Limits{MaxPlayers: 8}).CheckNew(home) != nil {
		t.Fatal("zero is a cap")
	}
}

func TestNewSessionsPerAddress(t *testing.T) {
	g := NewGate(Limits{NewSessionsPerAddressPerMonth: 2})
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
	if g.MayIssue(first, start.Add(359*time.Hour)) || !g.MayIssue(first, start.Add(361*time.Hour)) {
		t.Fatal("the allowance does not refill at its monthly rate")
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

	g.Prune(start.Add(2 * month))
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
