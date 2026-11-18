package metrics

import (
	"context"
	"log/slog"
	"sync"
	"sync/atomic"
	"time"
)

// Why a connection ended.
const (
	ReasonLeft          = "left"
	ReasonDropped       = "dropped"
	ReasonTimeout       = "timeout"
	ReasonKicked        = "kicked"
	ReasonBanned        = "banned"
	ReasonReplaced      = "replaced"
	ReasonRateLimited   = "rate_limited"
	ReasonBacklog       = "backlog"
	ReasonServerStopped = "server_stopped"
	ReasonServerLost    = "server_lost"
)

// Why a connection was refused before it joined.
const (
	RefusedJoiningBusy        = "joining_busy"
	RefusedNoHello            = "no_hello"
	RefusedInvalidHello       = "invalid_hello"
	RefusedProtocolMismatch   = "protocol_mismatch"
	RefusedDisplayNameInvalid = "display_name_invalid"
	RefusedDisplayNameTaken   = "display_name_taken"
	RefusedBanned             = "banned"
	RefusedTooManyNewSessions = "too_many_new_sessions"
	RefusedServerFull         = "server_full"
	RefusedAddressFull        = "address_full"
	RefusedServerError        = "server_error"
)

// Gauges are read once a sample. Counters are totals since the server started.
type Gauges struct {
	Players, InWorld  int
	BytesIn, BytesOut uint64
	LoopBusy          time.Duration
	// The longest hub loop pass since the previous read.
	LoopWorst time.Duration
}

const (
	sampleInterval     = time.Minute
	minimumSampleWidth = 5 * time.Second
	flushInterval      = time.Second
	purgeInterval      = time.Hour
	eventQueue         = 4096
)

type event struct {
	id          uint64
	session     uint64
	displayName string
	at          time.Time
	// Empty for a connection that started.
	reason string
}

// Recorder takes events from connection goroutines without waiting on the database, and writes
// them and a sample a minute on its own goroutine. A nil Recorder records nothing.
type Recorder struct {
	store   *Store
	log     *slog.Logger
	nextID  atomic.Uint64
	events  chan event
	dropped atomic.Uint64

	mu       sync.Mutex
	refusals map[string]int
}

func NewRecorder(store *Store, log *slog.Logger) (*Recorder, error) {
	closed, err := store.closeAbandoned()
	if err != nil {
		return nil, err
	}
	if closed > 0 {
		log.Warn("connections left open by an earlier run ended at its last sample", "count", closed)
	}
	last, err := store.lastConnectionID()
	if err != nil {
		return nil, err
	}
	r := &Recorder{store: store, log: log, events: make(chan event, eventQueue), refusals: make(map[string]int)}
	r.nextID.Store(last)
	return r, nil
}

// Connected returns the connection's ID for Ended.
func (r *Recorder) Connected(session uint64, displayName string, at time.Time) uint64 {
	if r == nil {
		return 0
	}
	id := r.nextID.Add(1)
	r.queue(event{id: id, session: session, displayName: displayName, at: at})
	return id
}

func (r *Recorder) Ended(id uint64, at time.Time, reason string) {
	if r == nil || id == 0 {
		return
	}
	r.queue(event{id: id, at: at, reason: reason})
}

func (r *Recorder) Refused(reason string) {
	if r == nil {
		return
	}
	r.mu.Lock()
	r.refusals[reason]++
	r.mu.Unlock()
}

func (r *Recorder) queue(e event) {
	select {
	case r.events <- e:
	default:
		r.dropped.Add(1)
	}
}

// Run writes until ctx ends, then ends every connection still open as stopped by the server.
// probe runs on this goroutine, and a sample is skipped while it fails.
func (r *Recorder) Run(ctx context.Context, probe func() (Gauges, bool)) {
	flush := time.NewTicker(flushInterval)
	defer flush.Stop()
	windowStart := time.Now()
	baseline, _ := probe()
	sampleTimer := time.NewTimer(time.Until(windowStart.Truncate(sampleInterval).Add(sampleInterval)))
	defer sampleTimer.Stop()
	lastPurge := time.Time{}
	var pending []event
	for {
		select {
		case <-ctx.Done():
			pending = r.drain(pending)
			r.write(pending)
			now := time.Now()
			if current, ok := probe(); ok && now.Sub(windowStart) >= minimumSampleWidth {
				r.sample(windowStart, now, baseline, current)
			} else if err := r.store.addRefusals(now, r.takeRefusals()); err != nil {
				r.log.Error("cannot write the refusals", "error", err)
			}
			if err := r.store.closeOpen(time.Now(), ReasonServerStopped); err != nil {
				r.log.Error("cannot end the open connections in the metrics database", "error", err)
			}
			return
		case e := <-r.events:
			pending = append(pending, e)
		case <-flush.C:
			pending = r.write(r.drain(pending))
		case now := <-sampleTimer.C:
			sampleTimer.Reset(time.Until(now.Truncate(sampleInterval).Add(sampleInterval)))
			pending = r.write(r.drain(pending))
			if now.Sub(windowStart) < minimumSampleWidth {
				continue
			}
			current, ok := probe()
			if !ok {
				continue
			}
			r.sample(windowStart, now, baseline, current)
			windowStart, baseline = now, current
			if now.Sub(lastPurge) >= purgeInterval {
				lastPurge = now
				if err := r.store.purge(now); err != nil {
					r.log.Error("cannot delete expired metrics", "error", err)
				}
			}
		}
	}
}

func (r *Recorder) drain(pending []event) []event {
	for {
		select {
		case e := <-r.events:
			pending = append(pending, e)
		default:
			return pending
		}
	}
}

func (r *Recorder) write(pending []event) []event {
	if dropped := r.dropped.Swap(0); dropped > 0 {
		r.log.Warn("metrics events dropped; the queue was full", "count", dropped)
	}
	if len(pending) == 0 {
		return pending
	}
	if err := r.store.writeEvents(pending); err != nil {
		r.log.Error("cannot write connection metrics", "error", err, "events", len(pending))
	}
	return pending[:0]
}

func (r *Recorder) takeRefusals() map[string]int {
	r.mu.Lock()
	defer r.mu.Unlock()
	refusals := r.refusals
	r.refusals = make(map[string]int)
	return refusals
}

func (r *Recorder) sample(from, to time.Time, previous, current Gauges) {
	refusals := r.takeRefusals()
	v := sample{
		// The window starts at most a few milliseconds after its minute.
		at:       from.Truncate(sampleInterval),
		seconds:  to.Sub(from).Seconds(),
		players:  current.Players,
		inWorld:  current.InWorld,
		bytesIn:  current.BytesIn - previous.BytesIn,
		bytesOut: current.BytesOut - previous.BytesOut,
		busyMs:   float64(current.LoopBusy-previous.LoopBusy) / float64(time.Millisecond),
		worstMs:  float64(current.LoopWorst) / float64(time.Millisecond),
	}
	if err := r.store.writeSample(v, refusals); err != nil {
		r.log.Error("cannot write the metrics sample", "error", err)
	}
}
