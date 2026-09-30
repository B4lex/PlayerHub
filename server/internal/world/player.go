// Package world holds each player's latest accepted state and the ordering
// rules that decide whether a new message is accepted.
package world

import (
	"errors"
	"time"

	"skyrim-mp/server/internal/validate"
	"skyrim-mp/server/internal/wire"
	"skyrim-mp/server/internal/wire/pb"
)

type Kind uint8

const (
	KindClient Kind = iota
	// A panel replay of a recording.
	KindReplay
	// A load-test bot replaying a recording inside the server.
	KindBot
)

var (
	ErrNotInWorld       = errors.New("not in world")
	ErrNoContext        = errors.New("no context")
	ErrStaleSequence    = errors.New("stale sequence")
	ErrStaleGraphEpoch  = errors.New("stale graph epoch")
	ErrStaleRevision    = errors.New("stale revision")
	ErrIncompatibleStep = errors.New("incompatible animation checkpoint")
	ErrActionGap        = errors.New("non-contiguous action sequence")
)

// Player is a connected or virtual player's accepted state. A player is in
// the world between Enter and Exit, and placed once it also has a context and
// a checkpoint.
type Player struct {
	Slot        uint32
	Kind        Kind
	ConnectedAt time.Time

	InWorld    bool
	Context    *pb.Context
	Checkpoint *pb.Checkpoint
	// Checkpoint encoded once as a PeerStates entry.
	Entry          []byte
	ActionSequence uint64
	// The newest checkpoint sequence since entering, kept when a space change
	// drops the checkpoint, so sequences still rise.
	lastSequence uint64

	Appearance *pb.Appearance
	NameIndex  uint32
	Equipment  *pb.Equipment
}

func (p *Player) Placed() bool {
	return p.InWorld && p.Context != nil && p.Checkpoint != nil
}

// Enter reports whether the player was out of the world.
func (p *Player) Enter() bool {
	if p.InWorld {
		return false
	}
	p.InWorld = true
	return true
}

// Exit drops the player's world state and reports whether it was in the
// world. Appearance and equipment are kept.
func (p *Player) Exit() bool {
	was := p.InWorld
	p.InWorld = false
	p.Context = nil
	p.Checkpoint = nil
	p.Entry = nil
	p.ActionSequence = 0
	p.lastSequence = 0
	return was
}

// ContextChange says what a new context changed.
type ContextChange uint8

const (
	ContextUnchanged ContextChange = iota
	// Graph epoch or furniture.
	ContextChanged
	// Cell or worldspace. The stored checkpoint belonged to the old space, so
	// it is dropped and the player is unplaced until its next checkpoint.
	ContextMoved
)

func (p *Player) SetContext(c *pb.Context) (ContextChange, error) {
	if !p.InWorld {
		return ContextUnchanged, ErrNotInWorld
	}
	change := ContextChanged
	if p.Context != nil {
		if c.GetGraphEpoch() < p.Context.GetGraphEpoch() {
			return ContextUnchanged, ErrStaleGraphEpoch
		}
		if sameContext(p.Context, c) {
			return ContextUnchanged, nil
		}
		if p.Context.GetCellId() != c.GetCellId() && (p.Context.GetWorldspaceId() == 0 || c.GetWorldspaceId() == 0) ||
			p.Context.GetWorldspaceId() != c.GetWorldspaceId() {
			change = ContextMoved
			p.Checkpoint = nil
			p.Entry = nil
		}
	}
	p.Context = c
	return change, nil
}

func (p *Player) SetCheckpoint(c *pb.Checkpoint) error {
	if !p.InWorld {
		return ErrNotInWorld
	}
	if p.Context == nil {
		return ErrNoContext
	}
	if p.lastSequence != 0 && c.GetSequence() <= p.lastSequence {
		return ErrStaleSequence
	}
	p.lastSequence = c.GetSequence()
	// The sender's slot, as every relayed copy carries it.
	c.Player = p.Slot
	p.Checkpoint = c
	p.Entry = wire.AppendEntry(nil, p.Slot, c)
	return nil
}

// AcceptAction checks the action against the latest checkpoint and context and
// advances the action sequence. The first action a server sees sets the
// boundary.
func (p *Player) AcceptAction(a *pb.Action) error {
	if !p.Placed() || a.GetGraphEpoch() != p.Context.GetGraphEpoch() ||
		a.GetCheckpointSequence() > p.Checkpoint.GetSequence() ||
		!validate.WeaponDrawn(p.Checkpoint) || p.Context.Furniture != nil {
		return ErrIncompatibleStep
	}
	if p.ActionSequence != 0 && a.GetActionSequence() != p.ActionSequence+1 {
		return ErrActionGap
	}
	p.ActionSequence = a.GetActionSequence()
	return nil
}

func (p *Player) SetAppearance(a *pb.Appearance, names *Names) error {
	if p.Appearance != nil && a.GetRevision() <= p.Appearance.GetRevision() {
		return ErrStaleRevision
	}
	p.Appearance = a
	p.NameIndex = names.Set(p.Slot, a.GetName())
	return nil
}

func (p *Player) SetEquipment(e *pb.Equipment) error {
	if p.Equipment != nil && e.GetRevision() <= p.Equipment.GetRevision() {
		return ErrStaleRevision
	}
	p.Equipment = e
	return nil
}

func sameContext(a, b *pb.Context) bool {
	if a.GetCellId() != b.GetCellId() || a.GetWorldspaceId() != b.GetWorldspaceId() ||
		a.GetGraphEpoch() != b.GetGraphEpoch() || (a.Furniture == nil) != (b.Furniture == nil) {
		return false
	}
	return a.Furniture == nil ||
		(a.Furniture.GetPlugin() == b.Furniture.GetPlugin() && a.Furniture.GetFormId() == b.Furniture.GetFormId())
}
