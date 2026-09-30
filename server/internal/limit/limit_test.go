package limit

import (
	"testing"
	"time"
)

func TestTokenBucket(t *testing.T) {
	var b TokenBucket
	now := time.Unix(0, 0)
	for i := range 3 {
		if !b.Allow(now, 2, 3) {
			t.Fatalf("burst refused at %d", i)
		}
	}
	if b.Allow(now, 2, 3) {
		t.Fatal("allowed beyond the burst")
	}
	if !b.Allow(now.Add(500*time.Millisecond), 2, 3) {
		t.Fatal("refill refused")
	}
	if b.Allow(now.Add(500*time.Millisecond), 2, 3) {
		t.Fatal("refilled more than the rate")
	}
}
