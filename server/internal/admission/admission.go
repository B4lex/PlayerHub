// Package admission decides which connections may join: it caps the players
// connected at once, the connections still waiting for their hello, and the
// new sessions each address may take per hour. Admin replays and in-server
// bots do not join through it.
package admission

import (
	"net/http"
	"net/netip"
	"strings"
	"sync"
	"time"

	"skyrim-mp/server/internal/limit"
)

type Limits struct {
	// Zero is no cap.
	MaxPlayers       int
	MaxPendingHellos int
	// Zero is no limit. Also the burst, so an address may take this many at
	// once and then one every hour divided by it.
	NewSessionsPerAddressPerHour float64
}

// Gate is safe for concurrent use.
type Gate struct {
	limits Limits

	mu      sync.Mutex
	pending int
	// Admitted connections by session. A session that connects again is
	// briefly admitted twice, until its earlier connection leaves.
	players map[uint64]int
	issued  map[netip.Prefix]*limit.TokenBucket
}

func NewGate(limits Limits) *Gate {
	return &Gate{limits: limits, players: make(map[uint64]int), issued: make(map[netip.Prefix]*limit.TokenBucket)}
}

// BeginHello counts a connection that has not sent its hello yet, unless
// too many already wait. Every true needs one EndHello.
func (g *Gate) BeginHello() bool {
	g.mu.Lock()
	defer g.mu.Unlock()
	if g.pending >= g.limits.MaxPendingHellos {
		return false
	}
	g.pending++
	return true
}

func (g *Gate) EndHello() {
	g.mu.Lock()
	g.pending--
	g.mu.Unlock()
}

// Full reports whether a session that is not connected would be refused.
func (g *Gate) Full() bool {
	g.mu.Lock()
	defer g.mu.Unlock()
	return g.full()
}

func (g *Gate) full() bool {
	return g.limits.MaxPlayers > 0 && len(g.players) >= g.limits.MaxPlayers
}

// Admit counts a connection of session as a player, unless the server is full.
// A session that is connected already is always admitted, so a player can
// replace a connection that has not timed out yet. Every true needs one Leave.
func (g *Gate) Admit(session uint64) bool {
	g.mu.Lock()
	defer g.mu.Unlock()
	if g.players[session] == 0 && g.full() {
		return false
	}
	g.players[session]++
	return true
}

func (g *Gate) Leave(session uint64) {
	g.mu.Lock()
	defer g.mu.Unlock()
	if g.players[session]--; g.players[session] <= 0 {
		delete(g.players, session)
	}
}

// MayIssue takes one of the new sessions address may have this hour.
func (g *Gate) MayIssue(address netip.Addr, now time.Time) bool {
	rate := g.limits.NewSessionsPerAddressPerHour
	if rate <= 0 {
		return true
	}
	key := addressKey(address)
	g.mu.Lock()
	defer g.mu.Unlock()
	bucket := g.issued[key]
	if bucket == nil {
		bucket = &limit.TokenBucket{}
		g.issued[key] = bucket
	}
	return bucket.Allow(now, rate/3600, rate)
}

// Prune forgets addresses whose allowance has refilled.
func (g *Gate) Prune(now time.Time) {
	rate := g.limits.NewSessionsPerAddressPerHour
	g.mu.Lock()
	defer g.mu.Unlock()
	for key, bucket := range g.issued {
		if bucket.Full(now, rate/3600, rate) {
			delete(g.issued, key)
		}
	}
}

// addressKey groups an IPv6 address with its /64, which one subscriber
// usually holds whole.
func addressKey(address netip.Addr) netip.Prefix {
	address = address.Unmap()
	bits := 32
	if address.Is6() {
		bits = 64
	}
	key, _ := address.Prefix(bits)
	return key
}

// ClientAddress is the address r came from. Behind a trusted proxy it is the
// nearest X-Forwarded-For hop that no trusted proxy has: proxies append the
// address they received from, so earlier hops may be forged by the client.
func ClientAddress(r *http.Request, trusted []netip.Prefix) netip.Addr {
	peer, err := netip.ParseAddrPort(r.RemoteAddr)
	if err != nil {
		return netip.Addr{}
	}
	address := peer.Addr().Unmap()
	if !contains(trusted, address) {
		return address
	}
	forwarded := strings.Split(strings.Join(r.Header.Values("X-Forwarded-For"), ","), ",")
	for i := len(forwarded) - 1; i >= 0; i-- {
		hop, err := netip.ParseAddr(strings.TrimSpace(forwarded[i]))
		if err != nil {
			break
		}
		address = hop.Unmap()
		if !contains(trusted, address) {
			break
		}
	}
	return address
}

func contains(prefixes []netip.Prefix, address netip.Addr) bool {
	for _, prefix := range prefixes {
		if prefix.Contains(address) {
			return true
		}
	}
	return false
}
