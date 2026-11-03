package delivery

import (
	"bytes"
	"encoding/binary"
	"testing"
	"time"

	"google.golang.org/protobuf/proto"

	"playerhub/server/internal/validate"
	"playerhub/server/internal/wire"
	"playerhub/server/internal/wire/pb"
)

// skeleton holds bone 0 alone, at root translation x.
func skeleton(x int16) []byte {
	b := binary.AppendUvarint([]byte{validate.SkeletonLayoutHuman}, 1<<validate.SkeletonBoneCount-2)
	b = binary.LittleEndian.AppendUint16(b, uint16(x))
	return append(b, 0, 0, 0, 0, 7, 0, 0, 0)
}

func posed(peer uint32, sequence uint64) *wire.Entry {
	return wire.NewEntry(peer, &pb.Checkpoint{Sequence: sequence, Skeleton: skeleton(int16(sequence))})
}

// recipient resolves poses as a client does: each from the one it last received of the peer.
type recipient struct {
	poses map[uint32]*wire.SkeletonPose
}

// drain returns, per written posed entry, its sequence and layout, and fails on a pose that does
// not resolve to its sequence's.
func (r *recipient) drain(t *testing.T, o *Outbox) []string {
	t.Helper()
	var b Batch
	if ok, _ := o.Drain(&b, time.Now()); !ok {
		t.Fatal("drain refused")
	}
	var got []string
	err := b.Write(func(frame []byte) error {
		var m pb.ServerMessage
		if err := proto.Unmarshal(frame, &m); err != nil {
			return err
		}
		switch body := m.Message.(type) {
		case *pb.ServerMessage_PeerStates:
			for _, s := range body.PeerStates.States {
				if len(s.Skeleton) == 0 {
					continue
				}
				pose, ok := wire.ResolveSkeletonPose(s.Skeleton, r.poses[s.Player])
				if !ok || !bytes.Equal(pose.Append(nil), skeleton(int16(s.Sequence))) {
					t.Fatalf("pose of %s does not resolve", format(s.Player, s.Sequence))
				}
				r.poses[s.Player] = pose
				layout := "whole"
				if s.Skeleton[0] == validate.SkeletonLayoutHumanChange {
					layout = "change"
				}
				got = append(got, format(s.Player, s.Sequence)+" "+layout)
			}
		case *pb.ServerMessage_PeerLeave:
			delete(r.poses, body.PeerLeave.Slot)
		case *pb.ServerMessage_PeerAction:
		default:
			t.Fatalf("unexpected %T", body)
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	return got
}

func TestPosesAreWrittenAsChangesFromTheLastWrittenPose(t *testing.T) {
	o := NewOutbox(16)
	r := &recipient{poses: make(map[uint32]*wire.SkeletonPose)}
	o.SetCheckpoint(1, 1, posed(1, 1), time.Now())
	o.SetCheckpoint(2, 1, posed(2, 1), time.Now())
	expect(t, r.drain(t, o), "A:1 whole", "B:1 whole")

	// Replaced before it was written, 2 is no reference.
	o.SetCheckpoint(1, 2, posed(1, 2), time.Now())
	o.SetCheckpoint(1, 3, posed(1, 3), time.Now())
	o.SetCheckpoint(2, 2, wire.NewEntry(2, &pb.Checkpoint{Sequence: 2}), time.Now())
	expect(t, r.drain(t, o), "A:3 change")

	// The anchored entry is written first, so the latest one changes from it.
	o.SetCheckpoint(1, 4, posed(1, 4), time.Now())
	o.AppendAnchored(1, 4, 4, posed(1, 4), action(1, 1))
	o.SetCheckpoint(1, 5, posed(1, 5), time.Now())
	expect(t, r.drain(t, o), "A:4 change", "A:5 change")

	// A checkpoint without a pose leaves the reference.
	o.SetCheckpoint(2, 3, posed(2, 3), time.Now())
	expect(t, r.drain(t, o), "B:3 change")

	// A peer that leaves and enters again starts from a whole pose.
	o.Leave(1, leave(1))
	o.Enter(1, 5, leave(9))
	o.SetCheckpoint(1, 6, posed(1, 6), time.Now())
	expect(t, r.drain(t, o), "A:6 whole")
}
