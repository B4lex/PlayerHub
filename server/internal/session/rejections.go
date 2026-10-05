package session

import (
	"log/slog"
	"sync"
	"time"
)

const rejectionLogIntervalSeconds = 5

// rejectionLog reports rejections without flooding the log: the first
// one at once, later ones counted and summarized at most every few seconds.
// The reader and the hub both report, so it is locked.
type rejectionLog struct {
	mu      sync.Mutex
	log     *slog.Logger
	message string
	last    time.Time
	counts  map[string]int
}

func newRejectionLog(log *slog.Logger, message string) *rejectionLog {
	return &rejectionLog{log: log, message: message, counts: make(map[string]int)}
}

func (r *rejectionLog) note(reason string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.counts[reason]++
	now := time.Now()
	if now.Sub(r.last).Seconds() < rejectionLogIntervalSeconds {
		return
	}
	r.last = now
	for reason, count := range r.counts {
		r.log.Warn(r.message, "reason", reason, "count", count)
	}
	clear(r.counts)
}
