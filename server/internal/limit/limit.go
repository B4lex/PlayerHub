// Package limit holds rate limits shared by sessions and the hub.
package limit

import "time"

// TokenBucket allows rate per second on average and burst at once.
type TokenBucket struct {
	tokens  float64
	updated time.Time
}

func (b *TokenBucket) Allow(now time.Time, rate, burst float64) bool {
	if b.updated.IsZero() {
		b.tokens = burst
	} else {
		b.tokens = min(burst, b.tokens+now.Sub(b.updated).Seconds()*rate)
	}
	b.updated = now
	if b.tokens < 1 {
		return false
	}
	b.tokens--
	return true
}

// Full reports whether the bucket has refilled to burst by now, so it would
// behave like a new one and can be dropped.
func (b *TokenBucket) Full(now time.Time, rate, burst float64) bool {
	return b.updated.IsZero() || b.tokens+now.Sub(b.updated).Seconds()*rate >= burst
}
