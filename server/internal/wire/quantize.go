package wire

import (
	"math"

	"skyrim-mp/server/internal/wire/pb"
)

// Quantization shared with the plugin's src/protocol. Positions are whole
// game units, heading is 1/4096 of a turn, pitch is 1/1024 radian, speed is
// whole units per second, and direction is 1/256 of the graph's range.
const (
	HeadingSteps   = 4096
	PitchScale     = 1024
	DirectionSteps = 256
	MaximumPitch   = 1609
	MaximumSpeed   = 100000
)

// Sample is an unquantized checkpoint.
type Sample struct {
	X, Y, Z, Heading, Pitch, Speed, Direction float64
	MovementState                             pb.MovementState
	Flags                                     pb.CheckpointFlag
}

func Quantize(s Sample, sequence uint64) *pb.Checkpoint {
	heading := math.Mod(s.Heading, 2*math.Pi)
	if heading < 0 {
		heading += 2 * math.Pi
	}
	return &pb.Checkpoint{
		Sequence:  sequence,
		X:         roundInt32(s.X),
		Y:         roundInt32(s.Y),
		Z:         roundInt32(s.Z),
		Heading:   uint32(math.Round(heading/(2*math.Pi)*HeadingSteps)) % HeadingSteps,
		Pitch:     int32(max(-MaximumPitch, min(MaximumPitch, math.Round(s.Pitch*PitchScale)))),
		Speed:     uint32(max(0, min(MaximumSpeed, math.Round(s.Speed)))),
		Direction: uint32(math.Round(max(0, s.Direction)*DirectionSteps)) % DirectionSteps,
		Flags:     uint32(s.MovementState) | uint32(s.Flags),
	}
}

func roundInt32(v float64) int32 {
	return int32(max(math.MinInt32, min(math.MaxInt32, math.Round(v))))
}

// MovementState returns the movement state bits of a checkpoint.
func MovementState(c *pb.Checkpoint) pb.MovementState {
	return pb.MovementState(c.GetFlags() & 0x7)
}

// Moving reports whether the movement state is not idle.
func Moving(c *pb.Checkpoint) bool {
	state := MovementState(c)
	return state != pb.MovementState_MOVEMENT_STATE_IDLE && state != pb.MovementState_MOVEMENT_STATE_SNEAKING_IDLE
}
