package session

import (
	"bytes"
	"encoding/binary"
	"testing"

	"playerhub/server/internal/config"
	"playerhub/server/internal/validate"
	"playerhub/server/internal/wire"
	"playerhub/server/internal/wire/pb"
)

// testPose holds bone 0 alone, at root translation x.
func testPose(t *testing.T, x int16) *wire.SkeletonPose {
	t.Helper()
	b := binary.AppendUvarint([]byte{validate.SkeletonLayoutHuman}, 1<<validate.SkeletonBoneCount-2)
	b = binary.LittleEndian.AppendUint16(b, uint16(x))
	b = append(b, 0, 0, 0, 0, 7, 0, 0, 0)
	pose, ok := wire.ReadSkeletonPose(b)
	if !ok {
		t.Fatal("test pose does not read")
	}
	return pose
}

func posedCheckpoint(sequence uint64, skeleton []byte) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: &pb.Checkpoint{
		Sequence: sequence, X: 10, Skeleton: skeleton}}}
}

func TestPosesTravelAsChanges(t *testing.T) {
	s := startServer(t, config.Default())
	a, slotA := join(t, s, "Lydia")
	b, _ := join(t, s, "Serana")
	b.send(&pb.ClientMessage{Message: &pb.ClientMessage_PoseLimit{PoseLimit: &pb.PoseLimit{Players: 1}}})
	place(b, 0)

	a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Enter{Enter: &pb.Enter{}}})
	a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: &pb.Context{
		Worldspace: &pb.FormRef{Plugin: "Skyrim.esm", FormId: 60}, GraphEpoch: 1}}})
	var sent *wire.SkeletonPose
	for i := range 4 {
		pose := testPose(t, int16(i))
		skeleton := pose.AppendChange(nil, sent)
		if (sent != nil) != (skeleton[0] == validate.SkeletonLayoutHumanChange) {
			t.Fatalf("pose %d has layout %d", i, skeleton[0])
		}
		a.send(posedCheckpoint(uint64(i+1), skeleton))
		sent = pose
	}

	var received *wire.SkeletonPose
	for sequence := uint64(0); sequence < 4; {
		m := b.receive()
		if enter := m.GetPeerEnter(); enter.GetSlot() == slotA {
			if len(enter.GetState().GetSkeleton()) != 0 {
				t.Fatal("PeerEnter carries a pose")
			}
			received, sequence = nil, enter.GetState().GetSequence()
		}
		for _, state := range m.GetPeerStates().GetStates() {
			if state.GetPlayer() != slotA {
				continue
			}
			pose, ok := wire.ResolveSkeletonPose(state.GetSkeleton(), received)
			if !ok {
				t.Fatalf("pose of checkpoint %d does not resolve", state.GetSequence())
			}
			if want := testPose(t, int16(state.GetSequence()-1)); !bytes.Equal(pose.Append(nil), want.Append(nil)) {
				t.Fatalf("checkpoint %d carries another pose", state.GetSequence())
			}
			received, sequence = pose, state.GetSequence()
		}
	}
}

func TestUnresolvablePosesAreDropped(t *testing.T) {
	s := startServer(t, config.Default())
	a, _ := join(t, s, "Lydia")
	change := testPose(t, 1).AppendChange(nil, testPose(t, 0))
	a.send(posedCheckpoint(1, change))
	a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Ping{Ping: &pb.Ping{RequestId: 1}}})
	if a.receive().GetPong().GetRequestId() != 1 {
		t.Fatal("connection did not survive an unresolvable pose")
	}
}
