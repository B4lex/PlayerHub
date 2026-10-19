package wire

import (
	"testing"
	"time"

	"google.golang.org/protobuf/proto"

	"playerhub/server/internal/wire/pb"
)

func TestConcatenatedEntriesDecodeAsPeerStates(t *testing.T) {
	checkpoints := []*pb.Checkpoint{
		{Sequence: 1, X: -5, Y: 70000, Z: 0, Heading: 4095, Pitch: -1609, Speed: 350, Direction: 128, Flags: 0x1F, Time: 16383},
		{Sequence: 1 << 40},
		{Sequence: 9, X: -2147483648, Y: 2147483647, Pitch: 1609, Speed: 100000, Direction: 255, Flags: 0xFFE},
	}
	var entries []byte
	for i, c := range checkpoints {
		entries = AppendEntry(entries, uint32(i+1)*1000, c)
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
	c := &pb.Checkpoint{Player: 7, Sequence: 12, X: 3, Y: -4, Z: 5, Heading: 6, Pitch: -7, Speed: 8, Direction: 9, Flags: 10, Time: 11}
	generated, err := proto.MarshalOptions{Deterministic: true}.Marshal(&pb.PeerStates{States: []*pb.Checkpoint{c}})
	if err != nil {
		t.Fatal(err)
	}
	if got := AppendEntry(nil, 7, c); string(got) != string(generated) {
		t.Fatalf("hand-written %x, generated %x", got, generated)
	}
}

func TestQuantize(t *testing.T) {
	c := Quantize(Sample{X: 1.375, Y: -1.375, Z: 0.1, Heading: -0.0001, Pitch: 2, Speed: -1, Direction: 0.999,
		MovementState: pb.MovementState_MOVEMENT_STATE_SNEAKING_RUNNING,
		Flags:         pb.CheckpointFlag_CHECKPOINT_FLAG_MOUNTED}, 3)
	want := &pb.Checkpoint{Sequence: 3, X: 6, Y: -6, Z: 0, Heading: 0, Pitch: 1609, Speed: 0, Direction: 0,
		Flags: 6 | 1024}
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
