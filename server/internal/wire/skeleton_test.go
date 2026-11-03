package wire

import (
	"encoding/binary"
	"math/rand/v2"
	"testing"

	"playerhub/server/internal/validate"
	"playerhub/server/internal/wire/pb"
)

func randomPose(random *rand.Rand, present uint64) *SkeletonPose {
	p := &SkeletonPose{present: present}
	for axis := range p.translation {
		p.translation[axis] = int16(random.IntN(1 << 16))
	}
	for bone := range validate.SkeletonBoneCount {
		if p.has(bone) {
			p.rotations[bone].largest = uint8(random.IntN(4))
			for i := range p.rotations[bone].components {
				p.rotations[bone].components[i] = uint16(random.IntN(1 << validate.SkeletonComponentBits(bone)))
			}
		}
	}
	return p
}

// nudge moves each present rotation by up to step steps per component, as a pose moves between
// checkpoints, and leaves some bones as they were.
func nudge(random *rand.Rand, p *SkeletonPose, step int) *SkeletonPose {
	next := *p
	for axis := range next.translation {
		next.translation[axis] += int16(random.IntN(2*step+1) - step)
	}
	for bone := range validate.SkeletonBoneCount {
		if !next.has(bone) || random.IntN(4) == 0 {
			continue
		}
		maximum := 1<<validate.SkeletonComponentBits(bone) - 1
		for i, c := range next.rotations[bone].components {
			next.rotations[bone].components[i] = uint16(min(max(int(c)+random.IntN(2*step+1)-step, 0), maximum))
		}
	}
	return &next
}

func TestLayoutTwoRoundTrips(t *testing.T) {
	random := rand.New(rand.NewPCG(7, 8))
	for _, present := range []uint64{allSkeletonBones, 0, 1, allSkeletonBones &^ (1 | 1<<62), 1 << 40} {
		p := randomPose(random, present)
		b := p.Append(nil)
		if !validate.SkeletonPose(b) {
			t.Fatalf("invalid layout 2 for %x", present)
		}
		if got, ok := ReadSkeletonPose(b); !ok || *got != *p {
			t.Fatalf("round trip of %x differs", present)
		}
	}
	if len(randomPose(random, allSkeletonBones).Append(nil)) != 211 {
		t.Fatal("a full pose is not 211 bytes")
	}
}

func TestChangesResolveToThePose(t *testing.T) {
	random := rand.New(rand.NewPCG(9, 10))
	for _, step := range []int{0, 1, 3, 20, 200} {
		reference := randomPose(random, allSkeletonBones)
		for range 50 {
			pose := nudge(random, reference, step)
			b := pose.AppendChange(nil, reference)
			if len(b) > len(pose.Append(nil)) {
				t.Fatalf("step %d: %d bytes exceed layout 2", step, len(b))
			}
			if step <= 3 && b[0] != validate.SkeletonLayoutHumanChange {
				t.Fatalf("step %d: small changes sent whole", step)
			}
			resolved, ok := ResolveSkeletonPose(b, reference)
			if !ok || *resolved != *pose {
				t.Fatalf("step %d: layout %d does not resolve to the pose", step, b[0])
			}
			reference = pose
		}
	}
}

func TestChangesCoverBonesTheReferenceLacks(t *testing.T) {
	random := rand.New(rand.NewPCG(11, 12))
	reference := randomPose(random, allSkeletonBones&^(1<<5|1<<40))
	pose := nudge(random, reference, 1)
	pose.present = allSkeletonBones &^ (1 << 7)
	pose.rotations[7] = skeletonRotation{}
	pose.rotations[5] = skeletonRotation{largest: 2, components: [3]uint16{1, 2, 3}}
	pose.rotations[40] = skeletonRotation{largest: 1, components: [3]uint16{4, 5, 6}}
	pose.rotations[9].largest = (pose.rotations[9].largest + 1) % 4
	b := pose.AppendChange(nil, reference)
	if b[0] != validate.SkeletonLayoutHumanChange {
		t.Fatal("sent whole")
	}
	if resolved, ok := ResolveSkeletonPose(b, reference); !ok || *resolved != *pose {
		t.Fatal("does not resolve to the pose")
	}
}

func TestInvalidChangesAreRejected(t *testing.T) {
	random := rand.New(rand.NewPCG(13, 14))
	reference := randomPose(random, allSkeletonBones)
	pose := nudge(random, reference, 1)
	valid := pose.AppendChange(nil, reference)
	if _, ok := ResolveSkeletonPose(valid, reference); !ok {
		t.Fatal("valid change rejected")
	}
	lacking := *reference
	lacking.present &^= 1 << 3
	// A one-bone pose whose rotation is unchanged: flags 0 after a zero translation change.
	oneBone := func(bits ...byte) []byte {
		return append(binary.AppendUvarint([]byte{validate.SkeletonLayoutHumanChange}, allSkeletonBones&^1), bits...)
	}
	cases := map[string]struct {
		b         []byte
		reference *SkeletonPose
	}{
		"no reference":             {valid, nil},
		"unchanged absent bone":    {valid, &lacking},
		"trailing byte":            {append(append([]byte{}, valid...), 0), reference},
		"truncated":                {valid[:len(valid)-1], reference},
		"unknown layout":           {append([]byte{4}, valid[1:]...), reference},
		"long mask":                {append([]byte{validate.SkeletonLayoutHumanChange, 0x80, 0}, valid[2:]...), reference},
		"bone 63":                  {append(binary.AppendUvarint([]byte{validate.SkeletonLayoutHumanChange}, 1<<63), valid[2:]...), reference},
		"nonzero padding":          {oneBone(0b0001_1100 | 0b1000_0000), reference},
		"too many golomb zeros":    {oneBone(0, 0, 0, 0), reference},
		"oversized":                {append(oneBone(), make([]byte, validate.MaximumSkeletonPoseSize)...), reference},
		"translation out of range": {translationChange(reference, 40000), reference},
		"component out of range":   {componentChange(reference, 600), reference},
	}
	for name, c := range cases {
		if _, ok := ResolveSkeletonPose(c.b, c.reference); ok {
			t.Errorf("%s accepted", name)
		}
	}
	// The case above minus its padding bit is valid.
	if _, ok := ResolveSkeletonPose(oneBone(0b0001_1100), reference); !ok {
		t.Fatal("one-bone change rejected")
	}
}

// translationChange is a one-bone change moving the first translation axis by change.
func translationChange(reference *SkeletonPose, change int32) []byte {
	w := bitWriter{b: binary.AppendUvarint([]byte{validate.SkeletonLayoutHumanChange}, allSkeletonBones&^1)}
	w.write(0, golombOrderBits)
	w.golomb(change+int32(32767-int(reference.translation[0])), 0)
	w.golomb(0, 0)
	w.golomb(0, 0)
	w.write(0, 1)
	return w.flush()
}

// componentChange is a one-bone change moving bone 0's first component by change.
func componentChange(reference *SkeletonPose, change int32) []byte {
	w := bitWriter{b: binary.AppendUvarint([]byte{validate.SkeletonLayoutHumanChange}, allSkeletonBones&^1)}
	w.write(0, golombOrderBits)
	for range 3 {
		w.golomb(0, 0)
	}
	w.write(1, 1)
	w.write(0, 1)
	w.write(0, golombOrderBits)
	w.golomb(change, 0)
	w.golomb(0, 0)
	w.golomb(0, 0)
	return w.flush()
}

func TestGolombCodesRoundTrip(t *testing.T) {
	for k := range uint32(maximumGolombOrder + 1) {
		for _, v := range []int32{0, 1, -1, 2, -2, 7, -8, 511, -511, 65535, -65535} {
			w := bitWriter{}
			w.golomb(v, k)
			n := len(w.flush())
			if (golombBits(v, k)+7)/8 != n {
				t.Fatalf("golombBits(%d, %d) disagrees with %d bytes", v, k, n)
			}
			r := bitReader{b: w.b}
			if got, ok := r.golomb(k); !ok || got != v || !r.finished() {
				t.Fatalf("golomb(%d, %d) read %d", v, k, got)
			}
		}
	}
}

func TestPosedEntriesEncodeWithoutAllocating(t *testing.T) {
	random := rand.New(rand.NewPCG(15, 16))
	reference := randomPose(random, allSkeletonBones)
	entry := NewEntry(1, &pb.Checkpoint{Sequence: 1, Skeleton: nudge(random, reference, 2).Append(nil)})
	b := make([]byte, 0, 512)
	if allocations := testing.AllocsPerRun(100, func() { b = entry.Append(b[:0], reference) }); allocations != 0 {
		t.Fatalf("%v allocations per entry", allocations)
	}
}
