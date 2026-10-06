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
	MaxPlayers                   int
	MaxPendingHellos             int
	NewSessionsPerAddressPerHour float64
}

type Gate struct {
	limits Limits

	mu      sync.Mutex
	pending int
	// A session that connects again is briefly admitted twice, until its earlier connection
	// leaves.
	players map[uint64]int
	issued  map[netip.Prefix]*limit.TokenBucket
}

func NewGate(limits Limits) *Gate {
	return &Gate{limits: limits, players: make(map[uint64]int), issued: make(map[netip.Prefix]*limit.TokenBucket)}
}

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

func (g *Gate) Full() bool {
	g.mu.Lock()
	defer g.mu.Unlock()
	return g.full()
}

func (g *Gate) full() bool {
	return g.limits.MaxPlayers > 0 && len(g.players) >= g.limits.MaxPlayers
}

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

func addressKey(address netip.Addr) netip.Prefix {
	address = address.Unmap()
	bits := 32
	if address.Is6() {
		bits = 64
	}
	key, _ := address.Prefix(bits)
	return key
}

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
