package world

import (
	"errors"
	"time"

	"playerhub/server/internal/validate"
	"playerhub/server/internal/wire"
	"playerhub/server/internal/wire/pb"
)

type Kind uint8

const (
	KindClient Kind = iota
	KindReplay
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

type Player struct {
	Slot        uint32
	Kind        Kind
	ConnectedAt time.Time
	Session     uint64
	DisplayName string

	InWorld        bool
	Context        *pb.Context
	Checkpoint     *pb.Checkpoint
	Entry          []byte
	ActionSequence uint64
	lastSequence   uint64

	Appearance *pb.Appearance
	Equipment  *pb.Equipment
}

func (p *Player) Name() string {
	if p.Kind == KindClient {
		return p.DisplayName
	}
	return p.Appearance.GetName()
}

func (p *Player) Placed() bool {
	return p.InWorld && p.Context != nil && p.Checkpoint != nil
}

func (p *Player) Transformed() bool {
	return p.Context.GetShapeshift() != pb.Shapeshift_SHAPESHIFT_NONE
}

func (p *Player) Enter() bool {
	if p.InWorld {
		return false
	}
	p.InWorld = true
	return true
}

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

type ContextChange uint8

const (
	ContextUnchanged ContextChange = iota
	ContextChanged
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
		if !sameFormRef(p.Context.InteriorCell, c.InteriorCell) || !sameFormRef(p.Context.Worldspace, c.Worldspace) {
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
	if validate.VampireLordFlags(c) && p.Context.GetShapeshift() != pb.Shapeshift_SHAPESHIFT_VAMPIRE_LORD {
		return ErrIncompatibleStep
	}
	p.lastSequence = c.GetSequence()
	// The sender's slot, as every relayed copy carries it.
	c.Player = p.Slot
	p.Checkpoint = c
	p.Entry = wire.AppendEntry(nil, p.Slot, c)
	return nil
}

func (p *Player) AcceptAction(a *pb.Action) error {
	if !p.Placed() || a.GetGraphEpoch() != p.Context.GetGraphEpoch() ||
		a.GetCheckpointSequence() > p.Checkpoint.GetSequence() || p.Context.Furniture != nil {
		return ErrIncompatibleStep
	}
	// A beast form's own graph decides what it accepts, including actions with
	// claws sheathed, such as howling and feeding.
	if beast := a.GetBeast(); beast != pb.BeastAction_BEAST_ACTION_UNSPECIFIED {
		if !validate.SameBeastGraph(p.Context.GetShapeshift(), validate.BeastActionForm(beast)) {
			return ErrIncompatibleStep
		}
	} else if !validate.WeaponDrawn(p.Checkpoint) || p.Context.GetShapeshift() != pb.Shapeshift_SHAPESHIFT_NONE {
		return ErrIncompatibleStep
	}
	if p.ActionSequence != 0 && a.GetActionSequence() != p.ActionSequence+1 {
		return ErrActionGap
	}
	p.ActionSequence = a.GetActionSequence()
	return nil
}

func (p *Player) SetAppearance(a *pb.Appearance) error {
	if p.Appearance != nil && a.GetRevision() <= p.Appearance.GetRevision() {
		return ErrStaleRevision
	}
	if p.Kind == KindClient {
		a.Name = p.DisplayName
	}
	p.Appearance = a
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
	return sameFormRef(a.InteriorCell, b.InteriorCell) && sameFormRef(a.Worldspace, b.Worldspace) &&
		a.GetGraphEpoch() == b.GetGraphEpoch() && a.GetShapeshift() == b.GetShapeshift() &&
		sameFormRef(a.Furniture, b.Furniture)
}

// sameFormRef compares one client's references, which spell a plugin the same way each time.
func sameFormRef(a, b *pb.FormRef) bool {
	return (a == nil) == (b == nil) && a.GetPlugin() == b.GetPlugin() && a.GetFormId() == b.GetFormId()
}
