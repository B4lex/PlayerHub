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
	// The session a client connected with and its nickname; zero and empty
	// for a virtual player.
	Session  uint64
	Nickname string

	InWorld    bool
	Context    *pb.Context
	Checkpoint *pb.Checkpoint
	// Checkpoint encoded once as a PeerStates entry.
	Entry          []byte
	ActionSequence uint64
	// The newest checkpoint sequence since entering, kept when a space change
	// drops the checkpoint, so sequences still rise.
	lastSequence uint64

	// As peers receive it: the server sets a client's name and numbers the
	// revisions.
	Appearance *pb.Appearance
	// The newest revision the player itself sent.
	sourceAppearanceRevision uint64
	Equipment                *pb.Equipment
}

// Name is what peers call the player: a client's nickname, or a virtual
// player's recorded name.
func (p *Player) Name() string {
	if p.Kind == KindClient {
		return p.Nickname
	}
	return p.Appearance.GetName()
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

// SetAppearance stores a newer appearance, changing it into what peers
// receive. The server numbers the revisions, because a rename is one too.
func (p *Player) SetAppearance(a *pb.Appearance) error {
	if a.GetRevision() <= p.sourceAppearanceRevision {
		return ErrStaleRevision
	}
	p.sourceAppearanceRevision = a.GetRevision()
	a.Revision = p.Appearance.GetRevision() + 1
	if p.Kind == KindClient {
		a.Name = p.Nickname
	}
	p.Appearance = a
	return nil
}

// Rename changes a client's nickname and reports whether it has an
// appearance, which becomes a newer revision with the new name.
func (p *Player) Rename(nickname string) bool {
	p.Nickname = nickname
	if p.Appearance == nil {
		return false
	}
	p.Appearance.Name = nickname
	p.Appearance.Revision++
	return true
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
