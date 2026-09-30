// Package delivery holds what each recipient is about to receive and turns it
// into WebSocket messages.
package delivery

import (
	"slices"
	"sync"
	"sync/atomic"
	"time"
)

const (
	// A checkpoint held back by the budget is retried after this long.
	budgetRetry = 25 * time.Millisecond
	// The waiting time a checkpoint starts with under the budget: one tick at
	// the default send rate.
	priorityFloorSeconds = 0.025
)

type itemKind uint8

const (
	itemMessage itemKind = iota
	// A PeerStates entry, batched with adjacent entries when written.
	itemEntry
	// A latest-wins message superseded before it was written.
	itemRemoved
)

type item struct {
	kind  itemKind
	peer  uint32
	bytes []byte
}

// Replaceable names a latest-wins message kind that is not a checkpoint.
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
	// When the peer first had an unwritten checkpoint; a newer one keeps it,
	// so a peer held back by the budget gains priority while it waits.
	since time.Time
}

// due is a latest checkpoint the budget may write in this drain.
type due struct {
	peer     uint32
	bytes    []byte
	priority float64
}

// Outbox is one recipient's pending traffic. The hub fills it; the
// connection's writer drains it. Checkpoints are latest-wins per peer.
// Everything else is ordered, and an ordered message about a peer is never
// overtaken by, and never overtakes, that peer's checkpoints: appending one
// first moves the peer's pending checkpoint into the ordered stream.
type Outbox struct {
	mu          sync.Mutex
	ready       chan struct{}
	ordered     []item
	live        int
	replaceable map[replaceKey]int
	latest      map[uint32]latestEntry
	// The newest checkpoint sequence the recipient holds or will receive, per
	// subscribed peer.
	queued map[uint32]uint64
	// Far peers' checkpoints are written at most once per interval; a newer
	// one waits in latest meanwhile.
	intervals   map[uint32]time.Duration
	lastWritten map[uint32]time.Time
	// How much a waiting checkpoint of the peer counts: nearer peers more.
	weights    map[uint32]float64
	limit      int
	overflowed bool
	closed     bool

	// Nil without a budget. The allowance is this recipient's share, spent by
	// every write and refilled over time.
	budget      *Budget
	allowance   float64
	allowanceAt time.Time
	dues        []due

	// For the admin panel: bytes written, and checkpoints replaced by a newer
	// one before they were written.
	written  atomic.Uint64
	replaced atomic.Uint64
}

// NewOutbox makes an outbox that overflows beyond limit unsent ordered
// messages.
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

// UseBudget makes every drain spend this recipient's share of b. Call it
// before the outbox is used, and Close to give the share back.
func (o *Outbox) UseBudget(b *Budget) {
	o.mu.Lock()
	defer o.mu.Unlock()
	o.budget = b
	b.join()
}

// SetClass sets the peer's rate class: the least time between two written
// checkpoints (zero writes every one), and the weight of a waiting
// checkpoint under the budget.
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

// Ready receives a value after something was added since the last Drain.
func (o *Outbox) Ready() <-chan struct{} {
	return o.ready
}

// Signal wakes the writer. SetCheckpoint does not, so the hub signals once per
// tick after distributing every changed checkpoint.
func (o *Outbox) Signal() {
	select {
	case o.ready <- struct{}{}:
	default:
	}
}

// SetCheckpoint replaces the peer's pending checkpoint.
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

// AddWritten counts bytes the writer sent.
func (o *Outbox) AddWritten(bytes int) {
	o.written.Add(uint64(bytes))
}

// Counters returns the bytes written and the checkpoints replaced before
// being written, since the outbox was made.
func (o *Outbox) Counters() (written, replaced uint64) {
	return o.written.Load(), o.replaced.Load()
}

// Queued returns the newest checkpoint sequence of the peer that the
// recipient holds or will receive, or zero.
func (o *Outbox) Queued(peer uint32) uint64 {
	o.mu.Lock()
	defer o.mu.Unlock()
	return o.queued[peer]
}

// Enter appends a PeerEnter carrying the peer's checkpoint with the given
// sequence.
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

// Leave appends a PeerLeave. The peer's pending checkpoint is discarded.
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

// Append appends an ordered message about the peer, such as its context.
func (o *Outbox) Append(peer uint32, message []byte) {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.closed {
		return
	}
	o.moveLatestLocked(peer)
	o.appendLocked(item{itemMessage, peer, message})
}

// AppendAnchored appends a message that needs the peer's checkpoint with
// sequence anchor delivered first, such as an action. When the recipient
// holds nothing that new, entry (the peer's current checkpoint, with
// sequence) is sent in front of it.
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

// Replace appends a latest-wins message about the peer, dropping an unsent
// one of the same kind.
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

// AppendGlobal appends an ordered message about no peer: chat or a result.
func (o *Outbox) AppendGlobal(message []byte) {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.closed {
		return
	}
	o.appendLocked(item{itemMessage, 0, message})
}

// Close stops accepting traffic and wakes the writer.
func (o *Outbox) Close() {
	o.mu.Lock()
	if !o.closed && o.budget != nil {
		o.budget.leave()
	}
	o.closed = true
	o.mu.Unlock()
	o.Signal()
}

// Drain moves everything due at now into b. It returns false once the outbox
// is closed or its ordered backlog overflowed; the writer then closes the
// connection. next is when a deferred checkpoint falls due, or zero.
//
// Ordered messages always go. Under a budget, they are charged to it, and the
// latest-wins checkpoints go in order of waiting time times weight until the
// allowance is spent; the rest wait, keeping their waiting time.
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
		// Waiting time counts from one tick, so a near peer that just changed
		// outranks far ones until they have waited several times as long.
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

// spendLocked writes the dues the allowance pays for, highest priority
// first, and returns the ones left waiting.
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
