package clock

import (
	"sync"
	"time"
)

type Clock interface {
	Now() time.Time
}

type Real struct{}

func (Real) Now() time.Time {
	return time.Now()
}

type Fake struct {
	mu  sync.Mutex
	now time.Time
}

func NewFake() *Fake {
	return &Fake{now: time.Unix(1_800_000_000, 0)}
}

func (f *Fake) Now() time.Time {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.now
}

func (f *Fake) Advance(seconds float64) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.now = f.now.Add(time.Duration(seconds * float64(time.Second)))
}
