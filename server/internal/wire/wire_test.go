package wire

import (
	"testing"

	"google.golang.org/protobuf/proto"

	"skyrim-mp/server/internal/wire/pb"
)

func TestConcatenatedEntriesDecodeAsPeerStates(t *testing.T) {
	checkpoints := []*pb.Checkpoint{
		{Sequence: 1, X: -5, Y: 70000, Z: 0, Heading: 4095, Pitch: -1609, Speed: 350, Direction: 128, Flags: 0x1F},
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
	c := &pb.Checkpoint{Player: 7, Sequence: 12, X: 3, Y: -4, Z: 5, Heading: 6, Pitch: -7, Speed: 8, Direction: 9, Flags: 10}
	generated, err := proto.MarshalOptions{Deterministic: true}.Marshal(&pb.PeerStates{States: []*pb.Checkpoint{c}})
	if err != nil {
		t.Fatal(err)
	}
	if got := AppendEntry(nil, 7, c); string(got) != string(generated) {
		t.Fatalf("hand-written %x, generated %x", got, generated)
	}
}
