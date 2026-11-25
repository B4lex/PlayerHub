package wire

import (
	"bytes"
	"math/rand/v2"
	"testing"

	"playerhub/server/internal/validate"
)

// randomFace sets about a third of the weights.
func randomFace(random *rand.Rand) *FacePose {
	f := &FacePose{}
	for i := range f.weights {
		if random.IntN(3) == 0 {
			f.weights[i] = uint8(1 + random.IntN(255))
		}
	}
	return f
}

func TestFacesRoundTrip(t *testing.T) {
	random := rand.New(rand.NewPCG(21, 22))
	for range 200 {
		face := randomFace(random)
		b := face.Append(nil)
		if !validate.FacePose(b) || len(b) > validate.MaximumFacePoseSize {
			t.Fatalf("face %x does not validate", b)
		}
		if read, ok := ReadFacePose(b); !ok || *read != *face {
			t.Fatalf("face %x reads back as %v", b, read)
		}
	}
	var full FacePose
	for i := range full.weights {
		full.weights[i] = 255
	}
	if b := full.Append(nil); len(b) != validate.MaximumFacePoseSize {
		t.Fatalf("a full face takes %d bytes", len(b))
	}
}

func TestFaceChangesResolveToTheFace(t *testing.T) {
	random := rand.New(rand.NewPCG(23, 24))
	for range 200 {
		reference := randomFace(random)
		face := *reference
		for range random.IntN(4) {
			face.weights[random.IntN(len(face.weights))] = uint8(random.IntN(256))
		}
		b := face.AppendChange(nil, reference)
		if len(b) > len(face.Append(nil)) {
			t.Fatalf("change %x is larger than the whole face", b)
		}
		if resolved, ok := ResolveFacePose(b, reference); !ok || *resolved != face {
			t.Fatalf("change %x resolves to %v", b, resolved)
		}
	}
	reference := randomFace(random)
	if b := reference.AppendChange(nil, reference); !bytes.Equal(b, []byte{validate.FaceLayoutChange, 0}) &&
		!bytes.Equal(b, reference.Append(nil)) {
		t.Fatalf("an unchanged face is %x", b)
	}
}

func TestInvalidFacesAreRefused(t *testing.T) {
	reference := &FacePose{}
	reference.weights[0] = 9
	for _, b := range [][]byte{
		{validate.FaceLayoutWhole, 1},                                              // a weight missing
		{validate.FaceLayoutWhole, 1, 0},                                           // a zero weight in the mask
		{validate.FaceLayoutWhole, 0, 4},                                           // a trailing byte
		{validate.FaceLayoutWhole, 0x80, 0},                                        // not the shortest varint
		{validate.FaceLayoutWhole, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x02}, // bit 50
		{validate.FaceLayoutChange, 1, 9},                                          // unchanged weight written as a change
		{validate.FaceLayoutChange, 1},                                             // a weight missing
		{7},                                                                        // unknown layout
	} {
		if _, ok := ResolveFacePose(b, reference); ok {
			t.Errorf("face %x resolved", b)
		}
	}
	if _, ok := ResolveFacePose([]byte{validate.FaceLayoutChange, 0}, nil); ok {
		t.Error("a change resolved without a reference")
	}
	if _, ok := ResolveFacePose([]byte{validate.FaceLayoutChange, 1, 3}, reference); !ok {
		t.Error("a valid change was refused")
	}
}
