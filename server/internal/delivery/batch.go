package delivery

import "skyrim-mp/server/internal/wire"

// Batch is one drain of an outbox. A writer keeps one and reuses its buffers.
type Batch struct {
	ordered []item
	latest  [][]byte
	entries []byte
	frame   []byte
}

// Write passes each WebSocket message of the batch to write, in order.
// Adjacent checkpoint entries share one PeerStates, and the latest-wins
// checkpoints go last in one more. The slice passed to write is only valid
// during the call.
func (b *Batch) Write(write func([]byte) error) error {
	defer func() {
		// Drop references so written messages can be collected.
		clear(b.ordered)
		clear(b.latest)
	}()
	b.entries = b.entries[:0]
	for _, i := range b.ordered {
		switch i.kind {
		case itemEntry:
			b.entries = append(b.entries, i.bytes...)
		case itemMessage:
			if err := b.flushEntries(write); err != nil {
				return err
			}
			if err := write(i.bytes); err != nil {
				return err
			}
		}
	}
	for _, entry := range b.latest {
		b.entries = append(b.entries, entry...)
	}
	return b.flushEntries(write)
}

// Empty reports whether the batch has nothing to write.
func (b *Batch) Empty() bool {
	return len(b.ordered) == 0 && len(b.latest) == 0
}

func (b *Batch) flushEntries(write func([]byte) error) error {
	if len(b.entries) == 0 {
		return nil
	}
	b.frame = wire.AppendPeerStatesHeader(b.frame[:0], len(b.entries))
	b.frame = append(b.frame, b.entries...)
	b.entries = b.entries[:0]
	return write(b.frame)
}
