package admin

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"sync"
	"sync/atomic"
	"time"
)

const (
	feedIntervalSeconds  = 0.5
	feedKeepAliveSeconds = 15
)

// Feed pushes state snapshots to open panels as server-sent events, one on
// connect and one after changes, at most every feedIntervalSeconds. Changed
// is cheap enough to call for every accepted checkpoint: it does nothing
// without listeners or while a send is already due. The first change after a
// quiet period goes out at once; later ones collapse into one trailing send.
type Feed struct {
	build     func() State
	listeners atomic.Int32
	changed   chan struct{}

	mu          sync.Mutex
	subscribers map[chan []byte]struct{}
}

func NewFeed(build func() State) *Feed {
	return &Feed{build: build, changed: make(chan struct{}, 1), subscribers: make(map[chan []byte]struct{})}
}

func (f *Feed) Changed() {
	if f.listeners.Load() == 0 {
		return
	}
	select {
	case f.changed <- struct{}{}:
	default:
	}
}

func (f *Feed) Run(ctx context.Context) {
	interval := time.Duration(feedIntervalSeconds * float64(time.Second))
	var last time.Time
	for {
		select {
		case <-ctx.Done():
			return
		case <-f.changed:
		}
		if wait := time.Until(last.Add(interval)); wait > 0 {
			select {
			case <-ctx.Done():
				return
			case <-time.After(wait):
			}
		}
		// Changes during the wait are in this snapshot.
		select {
		case <-f.changed:
		default:
		}
		last = time.Now()
		data, err := json.Marshal(f.build())
		if err != nil {
			continue
		}
		f.mu.Lock()
		for subscriber := range f.subscribers {
			// Latest wins: a panel that has not taken the previous snapshot gets
			// this one instead.
			select {
			case <-subscriber:
			default:
			}
			subscriber <- data
		}
		f.mu.Unlock()
	}
}

func (f *Feed) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming unsupported", http.StatusInternalServerError)
		return
	}
	w.Header().Set("Content-Type", "text/event-stream")
	w.Header().Set("Cache-Control", "no-cache")
	subscriber := make(chan []byte, 1)
	f.mu.Lock()
	f.subscribers[subscriber] = struct{}{}
	f.mu.Unlock()
	f.listeners.Add(1)
	defer func() {
		f.listeners.Add(-1)
		f.mu.Lock()
		delete(f.subscribers, subscriber)
		f.mu.Unlock()
	}()

	initial, err := json.Marshal(f.build())
	if err != nil {
		return
	}
	keepAlive := time.NewTicker(feedKeepAliveSeconds * time.Second)
	defer keepAlive.Stop()
	send := func(data []byte) bool {
		_, err := fmt.Fprintf(w, "event: state\ndata: %s\n\n", data)
		flusher.Flush()
		return err == nil
	}
	if !send(initial) {
		return
	}
	for {
		select {
		case <-r.Context().Done():
			return
		case data := <-subscriber:
			if !send(data) {
				return
			}
		case <-keepAlive.C:
			if _, err := fmt.Fprint(w, ": keep-alive\n\n"); err != nil {
				return
			}
			flusher.Flush()
		}
	}
}
