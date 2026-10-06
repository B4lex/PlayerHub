package delivery

import (
	"sync"
	"time"
)

const (
	uplinkShare           = 0.8
	globalBurstSeconds    = 0.1
	recipientBurstSeconds = 0.2
	messageOverheadBytes  = 60
)

type Budget struct {
	mu           sync.Mutex
	rate         float64
	perRecipient float64
	tokens       float64
	updated      time.Time
	recipients   int
}

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

func (b *Budget) charge(now time.Time, bytes int) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.refillLocked(now)
	b.tokens -= float64(bytes)
}

func (b *Budget) take(now time.Time, want int) int {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.refillLocked(now)
	granted := min(float64(want), max(b.tokens, 0))
	b.tokens -= granted
	return int(granted)
}
