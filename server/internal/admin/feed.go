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
	feedChatQueue        = 256
)

type Feed struct {
	build     func() State
	listeners atomic.Int32
	changed   chan struct{}

	mu          sync.Mutex
	subscribers map[*subscriber]struct{}
}

type subscriber struct {
	state chan []byte
	chat  chan []byte
}

type ChatLine struct {
	AtMs     float64 `json:"atMs"`
	PlayerID string  `json:"playerId"`
	Name     string  `json:"name"`
	Text     string  `json:"text"`
}

func NewFeed(build func() State) *Feed {
	return &Feed{build: build, changed: make(chan struct{}, 1), subscribers: make(map[*subscriber]struct{})}
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

// Chat runs on the hub loop, so a panel that falls a queue behind loses lines instead of
// holding the hub.
func (f *Feed) Chat(slot uint32, name, text string) {
	if f.listeners.Load() == 0 {
		return
	}
	data, err := json.Marshal(ChatLine{AtMs: milliseconds(time.Now()), PlayerID: slotID(slot), Name: name, Text: text})
	if err != nil {
		return
	}
	f.mu.Lock()
	defer f.mu.Unlock()
	for s := range f.subscribers {
		select {
		case s.chat <- data:
		default:
		}
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
		for s := range f.subscribers {
			// Latest wins: a panel that has not taken the previous snapshot gets this one
			// instead.
			select {
			case <-s.state:
			default:
			}
			s.state <- data
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
	s := &subscriber{state: make(chan []byte, 1), chat: make(chan []byte, feedChatQueue)}
	f.mu.Lock()
	f.subscribers[s] = struct{}{}
	f.mu.Unlock()
	f.listeners.Add(1)
	defer func() {
		f.listeners.Add(-1)
		f.mu.Lock()
		delete(f.subscribers, s)
		f.mu.Unlock()
	}()

	initial, err := json.Marshal(f.build())
	if err != nil {
		return
	}
	keepAlive := time.NewTicker(feedKeepAliveSeconds * time.Second)
	defer keepAlive.Stop()
	send := func(event string, data []byte) bool {
		_, err := fmt.Fprintf(w, "event: %s\ndata: %s\n\n", event, data)
		flusher.Flush()
		return err == nil
	}
	if !send("state", initial) {
		return
	}
	for {
		select {
		case <-r.Context().Done():
			return
		case data := <-s.state:
			if !send("state", data) {
				return
			}
		case data := <-s.chat:
			if !send("chat", data) {
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
