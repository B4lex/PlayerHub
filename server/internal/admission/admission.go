package admission

import (
	"errors"
	"net/http"
	"net/netip"
	"strings"
	"sync"
	"time"

	"playerhub/server/internal/limit"
)

type Limits struct {
	MaxPlayers                    int
	MaxPlayersPerAddress          int
	MaxPendingHellos              int
	MaxPendingHellosPerAddress    int
	NewSessionsPerAddressPerMonth float64
}

const month = 30 * 24 * time.Hour

type Gate struct {
	limits Limits

	mu               sync.Mutex
	pending          int
	pendingByAddress map[netip.Prefix]int
	// A session that connects again is briefly admitted twice, until its earlier connection
	// leaves, so both maps count connections per session.
	players          map[uint64]int
	playersByAddress map[netip.Prefix]map[uint64]int
	issued           map[netip.Prefix]*limit.TokenBucket
}

func NewGate(limits Limits) *Gate {
	return &Gate{limits: limits, pendingByAddress: make(map[netip.Prefix]int), players: make(map[uint64]int),
		playersByAddress: make(map[netip.Prefix]map[uint64]int), issued: make(map[netip.Prefix]*limit.TokenBucket)}
}

var (
	ErrPendingHellos        = errors.New("too many connections waiting for their hello")
	ErrAddressPendingHellos = errors.New("too many connections from one address waiting for their hello")
	ErrServerFull           = errors.New("server full")
	ErrAddressFull          = errors.New("too many players from address")
)

func (g *Gate) BeginHello(address netip.Addr) error {
	key := AddressKey(address)
	g.mu.Lock()
	defer g.mu.Unlock()
	if perAddress := g.limits.MaxPendingHellosPerAddress; perAddress > 0 && g.pendingByAddress[key] >= perAddress {
		return ErrAddressPendingHellos
	}
	if g.pending >= g.limits.MaxPendingHellos {
		return ErrPendingHellos
	}
	g.pending++
	g.pendingByAddress[key]++
	return nil
}

func (g *Gate) EndHello(address netip.Addr) {
	key := AddressKey(address)
	g.mu.Lock()
	defer g.mu.Unlock()
	g.pending--
	if g.pendingByAddress[key]--; g.pendingByAddress[key] <= 0 {
		delete(g.pendingByAddress, key)
	}
}

// CheckNew refuses a session that cannot be admitted whatever it turns out to be, so a
// hello without a token is refused before it touches the store or spends the allowance.
func (g *Gate) CheckNew(address netip.Addr) error {
	key := AddressKey(address)
	g.mu.Lock()
	defer g.mu.Unlock()
	switch {
	case g.full():
		return ErrServerFull
	case g.addressFull(key):
		return ErrAddressFull
	}
	return nil
}

func (g *Gate) full() bool {
	return g.limits.MaxPlayers > 0 && len(g.players) >= g.limits.MaxPlayers
}

func (g *Gate) addressFull(key netip.Prefix) bool {
	return g.limits.MaxPlayersPerAddress > 0 && len(g.playersByAddress[key]) >= g.limits.MaxPlayersPerAddress
}

func (g *Gate) Admit(session uint64, address netip.Addr) error {
	key := AddressKey(address)
	g.mu.Lock()
	defer g.mu.Unlock()
	here := g.playersByAddress[key]
	switch {
	case g.players[session] == 0 && g.full():
		return ErrServerFull
	case here[session] == 0 && g.addressFull(key):
		return ErrAddressFull
	}
	g.players[session]++
	if here == nil {
		here = make(map[uint64]int)
		g.playersByAddress[key] = here
	}
	here[session]++
	return nil
}

func (g *Gate) Leave(session uint64, address netip.Addr) {
	key := AddressKey(address)
	g.mu.Lock()
	defer g.mu.Unlock()
	if g.players[session]--; g.players[session] <= 0 {
		delete(g.players, session)
	}
	here := g.playersByAddress[key]
	if here[session]--; here[session] <= 0 {
		delete(here, session)
	}
	if len(here) == 0 {
		delete(g.playersByAddress, key)
	}
}

func (g *Gate) MayIssue(address netip.Addr, now time.Time) bool {
	rate := g.limits.NewSessionsPerAddressPerMonth
	if rate <= 0 {
		return true
	}
	key := AddressKey(address)
	g.mu.Lock()
	defer g.mu.Unlock()
	bucket := g.issued[key]
	if bucket == nil {
		bucket = &limit.TokenBucket{}
		g.issued[key] = bucket
	}
	return bucket.Allow(now, rate/month.Seconds(), rate)
}

func (g *Gate) Prune(now time.Time) {
	rate := g.limits.NewSessionsPerAddressPerMonth
	g.mu.Lock()
	defer g.mu.Unlock()
	for key, bucket := range g.issued {
		if bucket.Full(now, rate/month.Seconds(), rate) {
			delete(g.issued, key)
		}
	}
}

func AddressKey(address netip.Addr) netip.Prefix {
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
