package delivery

import "playerhub/server/internal/wire"

type Batch struct {
	voice   [][]byte
	ordered []item
	latest  [][]byte
	entries []byte
	frame   []byte
}

func (b *Batch) Write(write func([]byte) error) error {
	defer func() {
		// Drop references so written messages can be collected.
		clear(b.voice)
		clear(b.ordered)
		clear(b.latest)
	}()
	// Voice goes first: it is played as it arrives, and nothing else waits on it.
	for _, message := range b.voice {
		if err := write(message); err != nil {
			return err
		}
	}
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
	return len(b.voice) == 0 && len(b.ordered) == 0 && len(b.latest) == 0
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
