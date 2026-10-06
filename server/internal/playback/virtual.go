package playback

import (
	"time"

	"google.golang.org/protobuf/proto"

	"skyrim-mp/server/internal/wire"
	"skyrim-mp/server/internal/wire/pb"
)

type VirtualPlayer struct {
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
