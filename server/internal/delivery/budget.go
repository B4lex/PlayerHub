package delivery

import (
	"sync"
	"time"
)

const (
	// Share of the uplink the budget uses. The rest is left for TCP
	// acknowledgements, retransmits, and headers, and so that the router's
	// upload buffer never sets the rate, which would raise every player's
	// latency.
	uplinkShare = 0.8
	// Burst of the global bucket and of each recipient's allowance, as
	// seconds of their rate.
	globalBurstSeconds    = 0.1
	recipientBurstSeconds = 0.2
	// Charged per WebSocket message besides its payload: IPv4, TCP with
	// timestamps, and the WebSocket header.
	messageOverheadBytes = 60
)

// Budget shares one uplink between recipients. Each recipient gets an equal
// share of the global rate, capped per recipient; what an idle recipient
// leaves unused stays in the global bucket for the others.
type Budget struct {
	mu           sync.Mutex
	rate         float64
	perRecipient float64
	tokens       float64
	updated      time.Time
	recipients   int
}

// NewBudget makes a budget for an uplink of bytes per second. perRecipient
// caps one recipient's share; zero leaves only the global share.
func NewBudget(uplinkBytesPerSecond, perRecipientBytesPerSecond float64) *Budget {
	return &Budget{rate: uplinkBytesPerSecond * uplinkShare, perRecipient: perRecipientBytesPerSecond}
}

func (b *Budget) join() {
	b.mu.Lock()
	b.recipients++
	b.mu.Unlock()
}

func (b *Budget) leave() {
	b.mu.Lock()
	b.recipients--
	b.mu.Unlock()
}

// share is one recipient's rate in bytes per second.
func (b *Budget) share() float64 {
	b.mu.Lock()
	defer b.mu.Unlock()
	share := b.rate / float64(max(b.recipients, 1))
	if b.perRecipient > 0 {
		share = min(share, b.perRecipient)
	}
	return share
}

func (b *Budget) refillLocked(now time.Time) {
	if b.updated.IsZero() {
		b.tokens = b.rate * globalBurstSeconds
	} else {
		b.tokens = min(b.rate*globalBurstSeconds, b.tokens+now.Sub(b.updated).Seconds()*b.rate)
	}
	b.updated = now
}

// charge takes bytes that are sent whatever the budget, such as ordered
// messages. The bucket may go negative, which holds back checkpoints.
func (b *Budget) charge(now time.Time, bytes int) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.refillLocked(now)
	b.tokens -= float64(bytes)
}

// take grants up to want bytes of checkpoints.
func (b *Budget) take(now time.Time, want int) int {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.refillLocked(now)
	granted := min(float64(want), max(b.tokens, 0))
	b.tokens -= granted
	return int(granted)
}
