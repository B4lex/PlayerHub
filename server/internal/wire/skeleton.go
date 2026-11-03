package wire

import (
	"encoding/binary"
	"math"
	"math/bits"

	"playerhub/server/internal/validate"
)

const (
	golombOrderBits     = 2
	maximumGolombOrder  = 1<<golombOrderBits - 1
	maximumGolombZeros  = 20 // a translation change takes at most 17
	allSkeletonBones    = 1<<validate.SkeletonBoneCount - 1
	skeletonTranslation = 3
)

// SkeletonPose is a skeleton pose as layout 2 stores it. A pose is shared and never changed
// once read; rotations of absent bones are unspecified.
type SkeletonPose struct {
	present     uint64
	translation [skeletonTranslation]int16
	rotations   [validate.SkeletonBoneCount]skeletonRotation
}

// skeletonRotation is a smallest-three quaternion: the index of its largest component and the
// other three in validate.SkeletonComponentBits steps.
type skeletonRotation struct {
	largest    uint8
	components [3]uint16
}

// ReadSkeletonPose reads layout 2.
func ReadSkeletonPose(b []byte) (*SkeletonPose, bool) {
	if len(b) == 0 || !validate.SkeletonPose(b) {
		return nil, false
	}
	missing, offset, _ := validate.SkeletonHeader(b, validate.SkeletonLayoutHuman)
	p := &SkeletonPose{present: allSkeletonBones &^ missing}
	for axis := range p.translation {
		p.translation[axis] = int16(binary.LittleEndian.Uint16(b[offset+2*axis:]))
	}
	r := bitReader{b: b, next: offset + 2*skeletonTranslation}
	for bone := range validate.SkeletonBoneCount {
		if p.has(bone) {
			p.rotations[bone] = r.rotation(validate.SkeletonComponentBits(bone))
		}
	}
	return p, true
}

// ResolveSkeletonPose reads layout 2, or layout 3 as a change from reference.
func ResolveSkeletonPose(b []byte, reference *SkeletonPose) (*SkeletonPose, bool) {
	if len(b) == 0 {
		return nil, false
	}
	if b[0] == validate.SkeletonLayoutHuman {
		return ReadSkeletonPose(b)
	}
	if b[0] != validate.SkeletonLayoutHumanChange || reference == nil || len(b) > validate.MaximumSkeletonPoseSize {
		return nil, false
	}
	missing, n, ok := validate.SkeletonHeader(b, validate.SkeletonLayoutHumanChange)
	if !ok {
		return nil, false
	}
	p := &SkeletonPose{present: allSkeletonBones &^ missing}
	r := bitReader{b: b, next: n}
	order := r.read(golombOrderBits)
	for axis := range p.translation {
		change, ok := r.golomb(order)
		if !ok {
			return nil, false
		}
		t := int64(reference.translation[axis]) + int64(change)
		if t < math.MinInt16 || t > math.MaxInt16 {
			return nil, false
		}
		p.translation[axis] = int16(t)
	}
	for bone := range validate.SkeletonBoneCount {
		if !p.has(bone) {
			continue
		}
		componentBits := validate.SkeletonComponentBits(bone)
		referenced := reference.has(bone)
		if r.read(1) == 0 {
			if !referenced {
				return nil, false
			}
			p.rotations[bone] = reference.rotations[bone]
			continue
		}
		if r.read(1) != 0 {
			p.rotations[bone] = r.rotation(componentBits)
			continue
		}
		if !referenced {
			return nil, false
		}
		rotation := reference.rotations[bone]
		order := r.read(golombOrderBits)
		for i, component := range rotation.components {
			change, ok := r.golomb(order)
			if !ok {
				return nil, false
			}
			v := int64(component) + int64(change)
			if v < 0 || v > 1<<componentBits-1 {
				return nil, false
			}
			rotation.components[i] = uint16(v)
		}
		p.rotations[bone] = rotation
	}
	if !r.finished() {
		return nil, false
	}
	return p, true
}

// Append appends p as layout 2.
func (p *SkeletonPose) Append(b []byte) []byte {
	b = append(b, validate.SkeletonLayoutHuman)
	b = binary.AppendUvarint(b, allSkeletonBones&^p.present)
	for _, t := range p.translation {
		b = binary.LittleEndian.AppendUint16(b, uint16(t))
	}
	w := bitWriter{b: b}
	for bone := range validate.SkeletonBoneCount {
		if p.has(bone) {
			w.rotation(p.rotations[bone], validate.SkeletonComponentBits(bone))
		}
	}
	return w.flush()
}

type boneChange uint8

const (
	boneAbsolute boneChange = iota
	boneUnchanged
	boneDelta
)

type bonePlan struct {
	change boneChange
	order  uint32
}

// AppendChange appends p as layout 3 against reference when that is smaller, else as layout 2.
func (p *SkeletonPose) AppendChange(b []byte, reference *SkeletonPose) []byte {
	start := len(b)
	b = p.Append(b)
	if reference == nil {
		return b
	}
	absolute := len(b) - start
	var translation [skeletonTranslation]int32
	for axis := range translation {
		translation[axis] = int32(p.translation[axis]) - int32(reference.translation[axis])
	}
	translationOrder, size := bestOrder(translation)
	size += golombOrderBits
	var plans [validate.SkeletonBoneCount]bonePlan
	for bone := range validate.SkeletonBoneCount {
		if !p.has(bone) {
			continue
		}
		// The changed and absolute flags, then the rotation as layout 2 stores it.
		planBits := 2 + 2 + 3*validate.SkeletonComponentBits(bone)
		if reference.has(bone) {
			rotation, previous := p.rotations[bone], reference.rotations[bone]
			switch {
			case rotation == previous:
				plans[bone].change = boneUnchanged
				planBits = 1
			case rotation.largest == previous.largest:
				order, changeBits := bestOrder(componentChanges(rotation, previous))
				if 2+golombOrderBits+changeBits < planBits {
					plans[bone] = bonePlan{boneDelta, order}
					planBits = 2 + golombOrderBits + changeBits
				}
			}
		}
		size += planBits
	}
	var mask [binary.MaxVarintLen64]byte
	header := 1 + binary.PutUvarint(mask[:], allSkeletonBones&^p.present)
	if header+(size+7)/8 >= absolute {
		return b
	}
	b = append(b[:start], validate.SkeletonLayoutHumanChange)
	b = binary.AppendUvarint(b, allSkeletonBones&^p.present)
	w := bitWriter{b: b}
	w.write(translationOrder, golombOrderBits)
	for _, change := range translation {
		w.golomb(change, translationOrder)
	}
	for bone := range validate.SkeletonBoneCount {
		if !p.has(bone) {
			continue
		}
		switch plan := plans[bone]; plan.change {
		case boneUnchanged:
			w.write(0, 1)
		case boneDelta:
			w.write(1, 1)
			w.write(0, 1)
			w.write(plan.order, golombOrderBits)
			for _, change := range componentChanges(p.rotations[bone], reference.rotations[bone]) {
				w.golomb(change, plan.order)
			}
		case boneAbsolute:
			w.write(1, 1)
			w.write(1, 1)
			w.rotation(p.rotations[bone], validate.SkeletonComponentBits(bone))
		}
	}
	return w.flush()
}

func (p *SkeletonPose) has(bone int) bool {
	return p.present>>bone&1 != 0
}

func componentChanges(rotation, reference skeletonRotation) [3]int32 {
	var changes [3]int32
	for i := range changes {
		changes[i] = int32(rotation.components[i]) - int32(reference.components[i])
	}
	return changes
}

func bestOrder(values [3]int32) (order uint32, size int) {
	size = math.MaxInt
	for k := range uint32(maximumGolombOrder + 1) {
		n := 0
		for _, v := range values {
			n += golombBits(v, k)
		}
		if n < size {
			order, size = k, n
		}
	}
	return order, size
}

func zigZag(v int32) uint32 {
	return uint32(v<<1) ^ uint32(v>>31)
}

func unZigZag(u uint32) int32 {
	return int32(u>>1) ^ -int32(u&1)
}

// golombBits is the size of v as a signed Exp-Golomb code of order k: zeros, a one, then the
// code's lower bits.
func golombBits(v int32, k uint32) int {
	width := bits.Len32(zigZag(v) + 1<<k)
	return 2*width - 1 - int(k)
}

// bitWriter writes least significant bit first, within and across bytes, at most 24 bits a call.
type bitWriter struct {
	b       []byte
	pending uint64
	count   uint
}

func (w *bitWriter) write(v uint32, n uint) {
	w.pending |= uint64(v&(1<<n-1)) << w.count
	w.count += n
	for w.count >= 8 {
		w.b = append(w.b, byte(w.pending))
		w.pending >>= 8
		w.count -= 8
	}
}

func (w *bitWriter) golomb(v int32, k uint32) {
	code := zigZag(v) + 1<<k
	width := uint(bits.Len32(code))
	w.write(0, width-1-uint(k))
	w.write(1, 1)
	w.write(code, width-1)
}

func (w *bitWriter) rotation(r skeletonRotation, componentBits int) {
	w.write(uint32(r.largest), 2)
	for _, c := range r.components {
		w.write(uint32(c), uint(componentBits))
	}
}

// flush pads the last byte with zero bits.
func (w *bitWriter) flush() []byte {
	if w.count > 0 {
		w.b = append(w.b, byte(w.pending))
		w.pending, w.count = 0, 0
	}
	return w.b
}

// bitReader reads zero bits past the end; finished tells whether it read exactly the bytes there are.
type bitReader struct {
	b       []byte
	next    int
	pending uint64
	count   uint
}

func (r *bitReader) read(n uint32) uint32 {
	for r.count < uint(n) {
		var next uint64
		if r.next < len(r.b) {
			next = uint64(r.b[r.next])
		}
		r.next++
		r.pending |= next << r.count
		r.count += 8
	}
	v := uint32(r.pending & (1<<n - 1))
	r.pending >>= n
	r.count -= uint(n)
	return v
}

func (r *bitReader) golomb(k uint32) (int32, bool) {
	zeros := uint32(0)
	for r.read(1) == 0 {
		if zeros++; zeros > maximumGolombZeros {
			return 0, false
		}
	}
	lower := zeros + k
	code := 1<<lower | r.read(lower)
	return unZigZag(code - 1<<k), true
}

func (r *bitReader) rotation(componentBits int) skeletonRotation {
	rotation := skeletonRotation{largest: uint8(r.read(2))}
	for i := range rotation.components {
		rotation.components[i] = uint16(r.read(uint32(componentBits)))
	}
	return rotation
}

// finished reports whether every byte was read, none past the end, and the last one's padding is zero.
func (r *bitReader) finished() bool {
	return r.next == len(r.b) && r.count < 8 && r.pending == 0
}
