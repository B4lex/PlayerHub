package hub

import (
	"context"
	"math"
	"sync/atomic"
	"time"
)

const sendPhaseSeconds = 0.005

type Loop struct {
	hub              *Hub
	work             chan func(*Hub)
	done             chan struct{}
	interval         time.Duration
	interestInterval time.Duration

	busy  atomic.Int64
	worst atomic.Int64
}

func NewLoop(h *Hub, sendRateHz float64) *Loop {
	phases := max(1, int(math.Round(1/sendRateHz/sendPhaseSeconds)))
	h.usePhases(phases)
	return &Loop{
		hub:              h,
		work:             make(chan func(*Hub), 8192),
		done:             make(chan struct{}),
		interval:         time.Duration(float64(time.Second) / sendRateHz / float64(phases)),
		interestInterval: time.Duration(float64(time.Second) / h.opts.Interest.UpdateHz),
	}
}

func (l *Loop) Run(ctx context.Context) {
	defer close(l.done)
	ticker := time.NewTicker(l.interval)
	defer ticker.Stop()
	interestTicker := time.NewTicker(l.interestInterval)
	defer interestTicker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case f := <-l.work:
			start := time.Now()
			f(l.hub)
			l.measure(start)
		case <-ticker.C:
			start := time.Now()
			l.hub.Tick()
			l.measure(start)
		case <-interestTicker.C:
			start := time.Now()
			l.hub.UpdateInterest()
			l.measure(start)
		}
	}
}

func (l *Loop) measure(start time.Time) {
	elapsed := int64(time.Since(start))
	l.busy.Add(elapsed)
	for {
		worst := l.worst.Load()
		if elapsed <= worst || l.worst.CompareAndSwap(worst, elapsed) {
			return
		}
	}
}

// Load gives the time the loop spent working since it started, and its longest pass since the
// previous call.
func (l *Loop) Load() (busy, worst time.Duration) {
	return time.Duration(l.busy.Load()), time.Duration(l.worst.Swap(0))
}

func (l *Loop) Submit(f func(*Hub)) bool {
	select {
	case l.work <- f:
		return true
	case <-l.done:
		return false
	}
}

func (l *Loop) Call(f func(*Hub)) bool {
	finished := make(chan struct{})
	if !l.Submit(func(h *Hub) { f(h); close(finished) }) {
		return false
	}
	select {
	case <-finished:
		return true
	case <-l.done:
		return false
	}
}
