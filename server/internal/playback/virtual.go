// Package playback replays recordings as virtual players that enter the hub
// through the same input as connections.
package playback

import (
	"time"

	"google.golang.org/protobuf/proto"

	"skyrim-mp/server/internal/wire"
	"skyrim-mp/server/internal/wire/pb"
)

// VirtualPlayer numbers one replayed player's messages. Receivers accept a
// peer's checkpoint only with a rising sequence, its context only with a
// non-decreasing graph epoch, its actions only when contiguous, and
// presentation only with rising revisions. A recording's own numbers break
// these rules once playback seeks back or loops, so the virtual player numbers
// everything itself and only borrows the recording's content. Every method
// returns a new message and never modifies the recorded one.
type VirtualPlayer struct {
	// Replaces the recorded name when not empty.
	Name string

	sequence           uint64
	actionSequence     uint64
	appearanceRevision uint64
	equipmentRevision  uint64
	epoch              uint64
	epochOffset        int64
	epochBreak         bool
	appearance         *pb.Appearance
	equipment          *pb.Equipment
}

// BreakContinuity marks that the next context does not follow from the
// previous one, as after a seek. It starts a new graph epoch, which makes
// receivers drop attacks queued for the old position.
func (v *VirtualPlayer) BreakContinuity() {
	v.epochBreak = true
}

func (v *VirtualPlayer) Context(recorded *pb.Context) *pb.Context {
	c := proto.Clone(recorded).(*pb.Context)
	recordedEpoch := int64(c.GetGraphEpoch())
	if v.epochBreak && v.epoch != 0 {
		v.epochOffset = int64(v.epoch) + 1 - recordedEpoch
	}
	v.epochBreak = false
	v.epoch = max(uint64(max(recordedEpoch+v.epochOffset, 1)), v.epoch)
	c.GraphEpoch = v.epoch
	return c
}

// Checkpoint numbers a recorded checkpoint and stamps it with sampled, since
// the recorded time does not follow on across a seek or a loop. hold stops
// all locomotion, which a paused replay shows: its actor stands still instead
// of running in place, keeping stance and hand state.
func (v *VirtualPlayer) Checkpoint(recorded *pb.Checkpoint, hold bool, sampled time.Time) *pb.Checkpoint {
	c := proto.Clone(recorded).(*pb.Checkpoint)
	v.sequence++
	c.Sequence = v.sequence
	c.Time = wire.SampleTime(sampled)
	c.Player = 0
	if hold {
		state := pb.MovementState(c.Flags & 0x7)
		idle := pb.MovementState_MOVEMENT_STATE_IDLE
		if state >= pb.MovementState_MOVEMENT_STATE_SNEAKING_IDLE {
			idle = pb.MovementState_MOVEMENT_STATE_SNEAKING_IDLE
		}
		c.Flags = c.Flags&^(0x7|uint32(pb.CheckpointFlag_CHECKPOINT_FLAG_JUMPING)) | uint32(idle)
		c.Speed = 0
	}
	return c
}

// Action numbers a recorded action against the latest checkpoint and
// context. It returns nil before the first checkpoint.
func (v *VirtualPlayer) Action(recorded *pb.Action) *pb.Action {
	if v.sequence == 0 || v.epoch == 0 {
		return nil
	}
	a := proto.Clone(recorded).(*pb.Action)
	v.actionSequence++
	a.ActionSequence = v.actionSequence
	a.GraphEpoch = v.epoch
	a.CheckpointSequence = v.sequence
	return a
}

// Appearance returns nil when the content equals what receivers already show,
// since they rebuild the actor for a new appearance.
func (v *VirtualPlayer) Appearance(recorded *pb.Appearance) *pb.Appearance {
	a := proto.Clone(recorded).(*pb.Appearance)
	if v.Name != "" {
		a.Name = v.Name
	}
	a.Revision = 0
	if v.appearance != nil && proto.Equal(a, v.appearance) {
		return nil
	}
	v.appearance = proto.Clone(a).(*pb.Appearance)
	v.appearanceRevision++
	a.Revision = v.appearanceRevision
	return a
}

func (v *VirtualPlayer) Equipment(recorded *pb.Equipment) *pb.Equipment {
	e := proto.Clone(recorded).(*pb.Equipment)
	e.Revision = 0
	if v.equipment != nil && proto.Equal(e, v.equipment) {
		return nil
	}
	v.equipment = proto.Clone(e).(*pb.Equipment)
	v.equipmentRevision++
	e.Revision = v.equipmentRevision
	return e
}
