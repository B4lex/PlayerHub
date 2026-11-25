package wire

import (
	"encoding/binary"
	"math/bits"

	"playerhub/server/internal/validate"
)

// FacePose is a face as the whole layout stores it, one byte per weight. A face is shared and
// never changed once read.
type FacePose struct {
	weights [validate.FaceWeightCount]uint8
}

// ReadFacePose reads the whole layout.
func ReadFacePose(b []byte) (*FacePose, bool) {
	if len(b) == 0 || !validate.FacePose(b) {
		return nil, false
	}
	mask, n, _ := validate.FaceHeader(b, validate.FaceLayoutWhole)
	f := &FacePose{}
	for i := range validate.FaceWeightCount {
		if mask>>i&1 != 0 {
			f.weights[i] = b[n]
			n++
		}
	}
	return f, true
}

// ResolveFacePose reads the whole layout, or a change from reference: the mask of the weights
// that differ from reference's and each one's new byte.
func ResolveFacePose(b []byte, reference *FacePose) (*FacePose, bool) {
	if len(b) == 0 {
		return nil, false
	}
	if b[0] == validate.FaceLayoutWhole {
		return ReadFacePose(b)
	}
	if b[0] != validate.FaceLayoutChange || reference == nil {
		return nil, false
	}
	mask, n, ok := validate.FaceHeader(b, validate.FaceLayoutChange)
	if !ok || len(b) != n+bits.OnesCount64(mask) {
		return nil, false
	}
	f := &FacePose{weights: reference.weights}
	for i := range validate.FaceWeightCount {
		if mask>>i&1 == 0 {
			continue
		}
		if b[n] == reference.weights[i] {
			return nil, false
		}
		f.weights[i] = b[n]
		n++
	}
	return f, true
}

// Append appends f as the whole layout.
func (f *FacePose) Append(b []byte) []byte {
	var mask uint64
	for i, w := range f.weights {
		if w != 0 {
			mask |= 1 << i
		}
	}
	return f.appendMasked(append(b, validate.FaceLayoutWhole), mask)
}

// AppendChange appends f as a change from reference when that is smaller, else whole.
func (f *FacePose) AppendChange(b []byte, reference *FacePose) []byte {
	if reference == nil {
		return f.Append(b)
	}
	var whole, changed uint64
	for i, w := range f.weights {
		if w != 0 {
			whole |= 1 << i
		}
		if w != reference.weights[i] {
			changed |= 1 << i
		}
	}
	if faceSize(changed) >= faceSize(whole) {
		return f.appendMasked(append(b, validate.FaceLayoutWhole), whole)
	}
	return f.appendMasked(append(b, validate.FaceLayoutChange), changed)
}

func (f *FacePose) appendMasked(b []byte, mask uint64) []byte {
	b = binary.AppendUvarint(b, mask)
	for i, w := range f.weights {
		if mask>>i&1 != 0 {
			b = append(b, w)
		}
	}
	return b
}

func faceSize(mask uint64) int {
	var varint [binary.MaxVarintLen64]byte
	return 1 + binary.PutUvarint(varint[:], mask) + bits.OnesCount64(mask)
}
