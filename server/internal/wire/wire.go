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

const (
	skeletonField protowire.Number = 13
	faceField     protowire.Number = 14
)

// Pose is what a posed entry carries, and leaves its recipient as the reference the player's
// next pose is written against: the skeleton pose and the face, nil when it carried none.
type Pose struct {
	Skeleton *SkeletonPose
	Face     *FacePose
}

// ResolvePose resolves a checkpoint's skeleton pose and face against the stream's reference.
// One that does not resolve leaves no reference.
func ResolvePose(skeleton, face []byte, reference Pose) (Pose, bool) {
	resolved, ok := ResolveSkeletonPose(skeleton, reference.Skeleton)
	if !ok {
		return Pose{}, false
	}
	p := Pose{Skeleton: resolved}
	if len(face) > 0 {
		if p.Face, ok = ResolveFacePose(face, reference.Face); !ok {
			return Pose{}, false
		}
	}
	return p, true
}

// Entry is one checkpoint as a PeerStates entry. A posed one is written per recipient, its
// skeleton pose and face as changes from the ones that recipient was last written of the player.
type Entry struct {
	// The whole entry, when it carries no pose.
	plain []byte
	// The entry's fields before the pose.
	fields []byte
	pose   Pose
}

// NewEntry reads c's pose as layout 2 and its face as whole; a pose that does not read is left
// out with the face.
func NewEntry(slot uint32, c *pb.Checkpoint) *Entry {
	if skeleton, ok := ReadSkeletonPose(c.GetSkeleton()); ok {
		face, _ := ReadFacePose(c.GetFace())
		return &Entry{fields: appendFields(nil, slot, c), pose: Pose{Skeleton: skeleton, Face: face}}
	}
	var fields [maximumFieldsSize]byte
	return &Entry{plain: appendEntry(nil, appendFields(fields[:0], slot, c))}
}

// Pose returns the entry's pose; ok is false when it carries none.
func (e *Entry) Pose() (pose Pose, ok bool) {
	return e.pose, e.pose.Skeleton != nil
}

// Plain returns the entry when it carries no pose, else nil.
func (e *Entry) Plain() []byte {
	return e.plain
}

// Append appends the entry for a recipient last written reference as the player's pose.
func (e *Entry) Append(b []byte, reference Pose) []byte {
	if e.pose.Skeleton == nil {
		return append(b, e.plain...)
	}
	// The pose goes first, since the lengths in front of it depend on its size, and then moves
	// behind them.
	start := len(b)
	b = e.pose.Skeleton.AppendChange(b, reference.Skeleton)
	skeleton := len(b) - start
	face, faceHeader := 0, 0
	if e.pose.Face != nil {
		b = e.pose.Face.AppendChange(b, reference.Face)
		face = len(b) - start - skeleton
		faceHeader = protowire.SizeTag(faceField) + protowire.SizeVarint(uint64(face))
	}
	skeletonHeader := protowire.SizeTag(skeletonField) + protowire.SizeVarint(uint64(skeleton))
	body := len(e.fields) + skeletonHeader + skeleton + faceHeader + face
	prefix := protowire.SizeTag(peerStatesEntryField) + protowire.SizeVarint(uint64(body)) + len(e.fields) +
		skeletonHeader
	b = slices.Grow(b, prefix+faceHeader)[:len(b)+prefix+faceHeader]
	// The face first, since the skeleton moves over where it was.
	faceStart := start + prefix + skeleton + faceHeader
	copy(b[faceStart:], b[start+skeleton:start+skeleton+face])
	copy(b[start+prefix:], b[start:start+skeleton])
	header := protowire.AppendTag(b[start:start], peerStatesEntryField, protowire.BytesType)
	header = protowire.AppendVarint(header, uint64(body))
	header = append(header, e.fields...)
	header = protowire.AppendTag(header, skeletonField, protowire.BytesType)
	protowire.AppendVarint(header, uint64(skeleton))
	if e.pose.Face != nil {
		header = protowire.AppendTag(b[faceStart-faceHeader:faceStart-faceHeader], faceField, protowire.BytesType)
		protowire.AppendVarint(header, uint64(face))
	}
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

// WithoutSkeleton returns a copy of c without its skeleton pose and face.
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
