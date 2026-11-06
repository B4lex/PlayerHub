package wire

import (
	"slices"

	"google.golang.org/protobuf/encoding/protowire"
	"google.golang.org/protobuf/proto"

	"playerhub/server/internal/wire/pb"
)

const (
	peerStatesField      protowire.Number = 3
	peerStatesEntryField protowire.Number = 1
)

const skeletonField protowire.Number = 13

// Entry is one checkpoint as a PeerStates entry. A posed one is written per recipient, its
// skeleton pose as a change from the one that recipient was last written of the player.
type Entry struct {
	// The whole entry, when it carries no pose.
	plain []byte
	// The entry's fields before the pose.
	fields []byte
	pose   *SkeletonPose
}

// NewEntry reads c's pose as layout 2; one that does not read is left out.
func NewEntry(slot uint32, c *pb.Checkpoint) *Entry {
	if pose, ok := ReadSkeletonPose(c.GetSkeleton()); ok {
		return &Entry{fields: appendFields(nil, slot, c), pose: pose}
	}
	var fields [maximumFieldsSize]byte
	return &Entry{plain: appendEntry(nil, appendFields(fields[:0], slot, c))}
}

func (e *Entry) Pose() *SkeletonPose {
	return e.pose
}

// Plain returns the entry when it carries no pose, else nil.
func (e *Entry) Plain() []byte {
	return e.plain
}

// Append appends the entry for a recipient last written reference as the player's pose.
func (e *Entry) Append(b []byte, reference *SkeletonPose) []byte {
	if e.pose == nil {
		return append(b, e.plain...)
	}
	// The pose goes first, since the lengths in front of it depend on its size, and then moves
	// behind them.
	start := len(b)
	b = e.pose.AppendChange(b, reference)
	pose := len(b) - start
	body := len(e.fields) + protowire.SizeTag(skeletonField) + protowire.SizeBytes(pose)
	prefix := protowire.SizeTag(peerStatesEntryField) + protowire.SizeVarint(uint64(body)) + body - pose
	b = slices.Grow(b, prefix)[:len(b)+prefix]
	copy(b[start+prefix:], b[start:start+pose])
	header := protowire.AppendTag(b[start:start], peerStatesEntryField, protowire.BytesType)
	header = protowire.AppendVarint(header, uint64(body))
	header = append(header, e.fields...)
	header = protowire.AppendTag(header, skeletonField, protowire.BytesType)
	protowire.AppendVarint(header, uint64(pose))
	return b
}

// The scalar fields take at most 64 bytes.
const maximumFieldsSize = 64

func appendFields(m []byte, slot uint32, c *pb.Checkpoint) []byte {
	m = appendVarint(m, 1, uint64(slot))
	m = appendVarint(m, 2, c.GetSequence())
	m = appendZigZag(m, 3, c.GetX())
	m = appendZigZag(m, 4, c.GetY())
	m = appendZigZag(m, 5, c.GetZ())
	m = appendVarint(m, 6, uint64(c.GetHeading()))
	m = appendZigZag(m, 7, c.GetPitch())
	m = appendVarint(m, 8, uint64(c.GetSpeed()))
	m = appendVarint(m, 9, uint64(c.GetDirection()))
	m = appendVarint(m, 10, uint64(c.GetFlags()))
	m = appendVarint(m, 11, uint64(c.GetTime()))
	return appendZigZag(m, 12, c.GetLookPitch())
}

func appendEntry(b, body []byte) []byte {
	b = protowire.AppendTag(b, peerStatesEntryField, protowire.BytesType)
	return protowire.AppendBytes(b, body)
}

// WithoutSkeleton returns a copy of c without its skeleton pose.
func WithoutSkeleton(c *pb.Checkpoint) *pb.Checkpoint {
	return &pb.Checkpoint{Player: c.Player, Sequence: c.Sequence, X: c.X, Y: c.Y, Z: c.Z, Heading: c.Heading,
		Pitch: c.Pitch, Speed: c.Speed, Direction: c.Direction, Flags: c.Flags, Time: c.Time, LookPitch: c.LookPitch}
}

func AppendPeerStatesHeader(b []byte, entriesLength int) []byte {
	b = protowire.AppendTag(b, peerStatesField, protowire.BytesType)
	return protowire.AppendVarint(b, uint64(entriesLength))
}

func Marshal(m *pb.ServerMessage) []byte {
	b, err := proto.MarshalOptions{Deterministic: true}.Marshal(m)
	if err != nil {
		// Only invalid UTF-8 fails, and every string was validated on ingest.
		panic(err)
	}
	return b
}

func appendVarint(b []byte, field protowire.Number, v uint64) []byte {
	if v == 0 {
		return b
	}
	b = protowire.AppendTag(b, field, protowire.VarintType)
	return protowire.AppendVarint(b, v)
}

func appendZigZag(b []byte, field protowire.Number, v int32) []byte {
	return appendVarint(b, field, protowire.EncodeZigZag(int64(v)))
}
