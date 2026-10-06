package delivery

import "playerhub/server/internal/wire"

type Batch struct {
	ordered []item
	latest  [][]byte
	entries []byte
	frame   []byte
}

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
