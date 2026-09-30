package hub

import (
	"context"
	"time"
)

// Loop runs a hub on one goroutine: submitted work in arrival order, a tick
// at the send rate, and the interest pass at its own rate. A slow tick is not
// caught up with a burst; the tickers drop what they missed.
type Loop struct {
	hub              *Hub
	work             chan func(*Hub)
	done             chan struct{}
	interval         time.Duration
	interestInterval time.Duration
}

func NewLoop(h *Hub, sendRateHz float64) *Loop {
	return &Loop{
		hub:              h,
		work:             make(chan func(*Hub), 8192),
		done:             make(chan struct{}),
		interval:         time.Duration(float64(time.Second) / sendRateHz),
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
			f(l.hub)
		case <-ticker.C:
			l.hub.Tick()
		case <-interestTicker.C:
			l.hub.UpdateInterest()
		}
	}
}

// Submit queues work and returns false once the loop has stopped. It blocks
// while the queue is full, which pushes back on the connection reading.
func (l *Loop) Submit(f func(*Hub)) bool {
	select {
	case l.work <- f:
		return true
	case <-l.done:
		return false
	}
}

// Call runs f on the hub goroutine and waits for it.
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
