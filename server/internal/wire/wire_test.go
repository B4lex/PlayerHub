package wire

import (
	"math/rand/v2"
	"testing"
	"time"

	"google.golang.org/protobuf/proto"

	"playerhub/server/internal/validate"
	"playerhub/server/internal/wire/pb"
)

func TestConcatenatedEntriesDecodeAsPeerStates(t *testing.T) {
	checkpoints := []*pb.Checkpoint{
		{Sequence: 1, X: -5, Y: 70000, Z: 0, Heading: 4095, Pitch: -1609, Speed: 350, Direction: 128, Flags: 0x1F, Time: 16383,
			LookPitch: 1609},
		{Sequence: 1 << 40},
		{Sequence: 9, X: -2147483648, Y: 2147483647, Pitch: 1609, Speed: 100000, Direction: 255, Flags: 0xFFE, LookPitch: -1609,
			Skeleton: randomPose(rand.New(rand.NewPCG(1, 2)), allSkeletonBones).Append(nil)},
	}
	var entries []byte
	for i, c := range checkpoints {
		entries = NewEntry(uint32(i+1)*1000, c).Append(entries, nil)
	}
	frame := AppendPeerStatesHeader(nil, len(entries))
	frame = append(frame, entries...)

	var decoded pb.ServerMessage
	if err := proto.Unmarshal(frame, &decoded); err != nil {
		t.Fatal(err)
	}
	states := decoded.GetPeerStates().GetStates()
	if len(states) != len(checkpoints) {
		t.Fatalf("got %d states", len(states))
	}
	for i, c := range checkpoints {
		want := proto.Clone(c).(*pb.Checkpoint)
		want.Player = uint32(i+1) * 1000
		if !proto.Equal(states[i], want) {
			t.Errorf("state %d = %v, want %v", i, states[i], want)
		}
	}
}

func TestEntryMatchesGeneratedEncoding(t *testing.T) {
	random := rand.New(rand.NewPCG(3, 4))
	for _, skeleton := range [][]byte{nil, randomPose(random, allSkeletonBones&^0b1011).Append(nil)} {
		c := &pb.Checkpoint{Player: 7, Sequence: 12, X: 3, Y: -4, Z: 5, Heading: 6, Pitch: -7, Speed: 8, Direction: 9,
			Flags: 10, Time: 11, LookPitch: -12, Skeleton: skeleton}
		generated, err := proto.MarshalOptions{Deterministic: true}.Marshal(&pb.PeerStates{States: []*pb.Checkpoint{c}})
		if err != nil {
			t.Fatal(err)
		}
		if got := NewEntry(7, c).Append(nil, nil); string(got) != string(generated) {
			t.Fatalf("hand-written %x, generated %x", got, generated)
		}
	}
}

func TestPosedEntryCarriesAChangeFromTheReference(t *testing.T) {
	random := rand.New(rand.NewPCG(5, 6))
	reference := randomPose(random, allSkeletonBones)
	pose := nudge(random, reference, 2)
	entry := NewEntry(3, &pb.Checkpoint{Sequence: 2, X: 40, Skeleton: pose.Append(nil)})
	entries := entry.Append(nil, reference)
	frame := append(AppendPeerStatesHeader(nil, len(entries)), entries...)
	var decoded pb.ServerMessage
	if err := proto.Unmarshal(frame, &decoded); err != nil {
		t.Fatal(err)
	}
	state := decoded.GetPeerStates().GetStates()[0]
	if state.GetPlayer() != 3 || state.GetSequence() != 2 || state.GetX() != 40 ||
		state.GetSkeleton()[0] != validate.SkeletonLayoutHumanChange {
		t.Fatalf("got %v", state)
	}
	if resolved, ok := ResolveSkeletonPose(state.GetSkeleton(), reference); !ok || *resolved != *pose {
		t.Fatal("the entry's pose does not resolve to the checkpoint's")
	}
}

func TestWithoutSkeletonKeepsEveryOtherField(t *testing.T) {
	c := &pb.Checkpoint{Player: 1, Sequence: 2, X: 3, Y: 4, Z: 5, Heading: 6, Pitch: 7, Speed: 8, Direction: 9, Flags: 10,
		Time: 11, LookPitch: 12, Skeleton: []byte{13}}
	want := proto.Clone(c).(*pb.Checkpoint)
	want.Skeleton = nil
	if got := WithoutSkeleton(c); !proto.Equal(got, want) {
		t.Fatalf("got %v, want %v", got, want)
	}
	// Every field is set above, so a new field must be added here and to WithoutSkeleton.
	if fields := c.ProtoReflect().Descriptor().Fields().Len(); fields != 13 {
		t.Fatalf("Checkpoint has %d fields", fields)
	}
}

func TestQuantize(t *testing.T) {
	c := Quantize(Sample{X: 1.375, Y: -1.375, Z: 0.1, Heading: -0.0001, Pitch: 2, Speed: -1, Direction: 0.999, LookPitch: -0.5,
		MovementState: pb.MovementState_MOVEMENT_STATE_SNEAKING_RUNNING,
		Flags:         pb.CheckpointFlag_CHECKPOINT_FLAG_MOUNTED}, 3)
	want := &pb.Checkpoint{Sequence: 3, X: 6, Y: -6, Z: 0, Heading: 0, Pitch: 1609, Speed: 0, Direction: 0,
		Flags: 6 | uint32(pb.CheckpointFlag_CHECKPOINT_FLAG_MOUNTED), LookPitch: -512}
	if !proto.Equal(c, want) {
		t.Fatalf("got %v, want %v", c, want)
	}
	if got := Quantize(Sample{Heading: 3.14159265, Direction: 0.25}, 1); got.Heading != 2048 || got.Direction != 64 {
		t.Fatalf("got %v", got)
	}
	if x, y, z := Position(&pb.Checkpoint{X: 6, Y: -6, Z: 1}); x != 1.5 || y != -1.5 || z != 0.25 {
		t.Fatalf("position %v %v %v", x, y, z)
	}
}

func TestSampleTimeWraps(t *testing.T) {
	base := time.UnixMilli(3 * TimeModulus)
	if got := SampleTime(base.Add(TimeModulus*time.Millisecond - time.Millisecond)); got != TimeModulus-1 {
		t.Fatalf("got %d before the wrap", got)
	}
	if got := SampleTime(base.Add(TimeModulus * time.Millisecond)); got != 0 {
		t.Fatalf("got %d at the wrap", got)
	}
}
