package delivery

import (
	"slices"
	"sync"
	"sync/atomic"
	"time"
)

const (
	budgetRetry          = 25 * time.Millisecond
	priorityFloorSeconds = 0.025
)

type itemKind uint8

const (
	itemMessage itemKind = iota
	itemEntry
	itemRemoved
)

type item struct {
	kind  itemKind
	peer  uint32
	bytes []byte
}

type Replaceable uint8

const (
	ReplaceAppearance Replaceable = iota
	ReplaceEquipment
)

type replaceKey struct {
	peer uint32
	kind Replaceable
}

type latestEntry struct {
	sequence uint64
	bytes    []byte
	since    time.Time
}

type due struct {
	peer     uint32
	bytes    []byte
	priority float64
}

type Outbox struct {
	mu          sync.Mutex
	ready       chan struct{}
	ordered     []item
	live        int
	replaceable map[replaceKey]int
	latest      map[uint32]latestEntry
	queued      map[uint32]uint64
	intervals   map[uint32]time.Duration
	lastWritten map[uint32]time.Time
	weights     map[uint32]float64
	limit       int
	overflowed  bool
	closed      bool

	budget      *Budget
	allowance   float64
	allowanceAt time.Time
	dues        []due

	written  atomic.Uint64
	replaced atomic.Uint64
}

func NewOutbox(limit int) *Outbox {
	return &Outbox{
		ready:       make(chan struct{}, 1),
		replaceable: make(map[replaceKey]int),
		latest:      make(map[uint32]latestEntry),
		queued:      make(map[uint32]uint64),
		intervals:   make(map[uint32]time.Duration),
		lastWritten: make(map[uint32]time.Time),
		weights:     make(map[uint32]float64),
		limit:       limit,
	}
}

func (o *Outbox) UseBudget(b *Budget) {
	o.mu.Lock()
	defer o.mu.Unlock()
	o.budget = b
	b.join()
}

func (o *Outbox) SetClass(peer uint32, interval time.Duration, weight float64) {
	o.mu.Lock()
	defer o.mu.Unlock()
	o.weights[peer] = weight
	if interval <= 0 {
		delete(o.intervals, peer)
		return
	}
	o.intervals[peer] = interval
}

func (o *Outbox) Ready() <-chan struct{} {
	return o.ready
}

func (o *Outbox) Signal() {
	select {
	case o.ready <- struct{}{}:
	default:
	}
}

func (o *Outbox) SetCheckpoint(peer uint32, sequence uint64, entry []byte, now time.Time) {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.closed {
		return
	}
	since := now
	if pending, ok := o.latest[peer]; ok {
		o.replaced.Add(1)
		since = pending.since
	}
	o.latest[peer] = latestEntry{sequence, entry, since}
	o.queued[peer] = sequence
}

func (o *Outbox) AddWritten(bytes int) {
	o.written.Add(uint64(bytes))
}

func (o *Outbox) Counters() (written, replaced uint64) {
	return o.written.Load(), o.replaced.Load()
}

func (o *Outbox) Queued(peer uint32) uint64 {
	o.mu.Lock()
	defer o.mu.Unlock()
	return o.queued[peer]
}

func (o *Outbox) Enter(peer uint32, sequence uint64, message []byte) {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.closed {
		return
	}
	delete(o.latest, peer)
	o.queued[peer] = sequence
	o.appendLocked(item{itemMessage, peer, message})
}

func (o *Outbox) Leave(peer uint32, message []byte) {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.closed {
		return
	}
	delete(o.latest, peer)
	delete(o.queued, peer)
	delete(o.intervals, peer)
	delete(o.lastWritten, peer)
	delete(o.weights, peer)
	o.appendLocked(item{itemMessage, peer, message})
}

func (o *Outbox) Append(peer uint32, message []byte) {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.closed {
		return
	}
	o.moveLatestLocked(peer)
	o.appendLocked(item{itemMessage, peer, message})
}

func (o *Outbox) AppendAnchored(peer uint32, anchor, sequence uint64, entry, message []byte) {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.closed {
		return
	}
	o.moveLatestLocked(peer)
	if o.queued[peer] < anchor {
		o.queued[peer] = sequence
		o.appendLocked(item{itemEntry, peer, entry})
	}
	o.appendLocked(item{itemMessage, peer, message})
}

func (o *Outbox) Replace(peer uint32, kind Replaceable, message []byte) {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.closed {
		return
	}
	key := replaceKey{peer, kind}
	if index, ok := o.replaceable[key]; ok {
		o.ordered[index] = item{kind: itemRemoved}
		o.live--
	}
	o.replaceable[key] = len(o.ordered)
	o.appendLocked(item{itemMessage, peer, message})
}

func (o *Outbox) AppendGlobal(message []byte) {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.closed {
		return
	}
	o.appendLocked(item{itemMessage, 0, message})
}

func (o *Outbox) Close() {
	o.mu.Lock()
	if !o.closed && o.budget != nil {
		o.budget.leave()
	}
	o.closed = true
	o.mu.Unlock()
	o.Signal()
}

func (o *Outbox) Drain(b *Batch, now time.Time) (ok bool, next time.Time) {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.closed || o.overflowed {
		return false, time.Time{}
	}
	b.ordered, o.ordered = o.ordered, b.ordered[:0]
	o.live = 0
	clear(o.replaceable)
	b.latest = b.latest[:0]
	dues := o.dues[:0]
	for peer, entry := range o.latest {
		if interval, far := o.intervals[peer]; far {
			if due := o.lastWritten[peer].Add(interval); now.Before(due) {
				if next.IsZero() || due.Before(next) {
					next = due
				}
				continue
			}
		}
		weight, ok := o.weights[peer]
		if !ok {
			weight = 1
		}
		priority := (now.Sub(entry.since).Seconds() + priorityFloorSeconds) * weight
		dues = append(dues, due{peer, entry.bytes, priority})
	}
	if o.budget != nil {
		if waiting := o.spendLocked(b, dues, now); len(waiting) > 0 &&
			(next.IsZero() || now.Add(budgetRetry).Before(next)) {
			next = now.Add(budgetRetry)
		}
	} else {
		for _, d := range dues {
			o.writeLatestLocked(b, d, now)
		}
	}
	clear(dues)
	o.dues = dues[:0]
	return true, next
}

func (o *Outbox) spendLocked(b *Batch, dues []due, now time.Time) []due {
	share := o.budget.share()
	if o.allowanceAt.IsZero() {
		o.allowance = share * recipientBurstSeconds
	} else {
		o.allowance = min(share*recipientBurstSeconds, o.allowance+now.Sub(o.allowanceAt).Seconds()*share)
	}
	o.allowanceAt = now
	orderedBytes := 0
	for _, i := range b.ordered {
		if i.kind == itemMessage {
			orderedBytes += len(i.bytes) + messageOverheadBytes
		} else {
			orderedBytes += len(i.bytes)
		}
	}
	if orderedBytes > 0 {
		o.allowance -= float64(orderedBytes)
		o.budget.charge(now, orderedBytes)
	}
	// The batch of latest checkpoints is one more message.
	want := messageOverheadBytes
	for _, d := range dues {
		want += len(d.bytes)
	}
	granted := o.budget.take(now, min(want, int(max(o.allowance, 0)))) - messageOverheadBytes
	slices.SortFunc(dues, func(a, b due) int {
		switch {
		case a.priority > b.priority:
			return -1
		case a.priority < b.priority:
			return 1
		}
		return int(a.peer) - int(b.peer)
	})
	spent := 0
	taken := 0
	for _, d := range dues {
		if spent+len(d.bytes) > granted {
			break
		}
		spent += len(d.bytes)
		o.writeLatestLocked(b, d, now)
		taken++
	}
	if taken > 0 {
		spent += messageOverheadBytes
	} else if granted > -messageOverheadBytes {
		// Nothing fit after all: give the batch's overhead back.
		o.budget.charge(now, -(granted + messageOverheadBytes))
	}
	o.allowance -= float64(spent)
	return dues[taken:]
}

func (o *Outbox) writeLatestLocked(b *Batch, d due, now time.Time) {
	b.latest = append(b.latest, d.bytes)
	delete(o.latest, d.peer)
	if _, far := o.intervals[d.peer]; far {
		o.lastWritten[d.peer] = now
	}
}

func (o *Outbox) moveLatestLocked(peer uint32) {
	if entry, ok := o.latest[peer]; ok {
		delete(o.latest, peer)
		o.appendLocked(item{itemEntry, peer, entry.bytes})
	}
}

func (o *Outbox) appendLocked(i item) {
	o.ordered = append(o.ordered, i)
	o.live++
	if o.live > o.limit {
		o.overflowed = true
	}
	o.Signal()
}
