// Package delivery holds what each recipient is about to receive and turns it
// into WebSocket messages.
package delivery

import (
	"sync"
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
	queued     map[uint32]uint64
	limit      int
	overflowed bool
	closed     bool
}

// NewOutbox makes an outbox that overflows beyond limit unsent ordered
// messages.
func NewOutbox(limit int) *Outbox {
	return &Outbox{
		ready:       make(chan struct{}, 1),
		replaceable: make(map[replaceKey]int),
		latest:      make(map[uint32]latestEntry),
		queued:      make(map[uint32]uint64),
		limit:       limit,
	}
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
func (o *Outbox) SetCheckpoint(peer uint32, sequence uint64, entry []byte) {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.closed {
		return
	}
	o.latest[peer] = latestEntry{sequence, entry}
	o.queued[peer] = sequence
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
	o.closed = true
	o.mu.Unlock()
	o.Signal()
}

// Drain moves everything pending into b. It returns false once the outbox is
// closed or its ordered backlog overflowed; the writer then closes the
// connection.
func (o *Outbox) Drain(b *Batch) bool {
	o.mu.Lock()
	defer o.mu.Unlock()
	if o.closed || o.overflowed {
		return false
	}
	b.ordered, o.ordered = o.ordered, b.ordered[:0]
	o.live = 0
	clear(o.replaceable)
	b.latest = b.latest[:0]
	for _, entry := range o.latest {
		b.latest = append(b.latest, entry.bytes)
	}
	clear(o.latest)
	return true
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
