package session

import (
	"fmt"
	"math"
	"strings"
	"testing"

	"google.golang.org/protobuf/proto"

	"playerhub/server/internal/validate"
	"playerhub/server/internal/wire/pb"
)

// largestAppearance fills every Appearance field to its validation limit with values
// whose encoding is longest.
func largestAppearance() *pb.ClientMessage {
	plugin := strings.Repeat("p", validate.MaximumPluginNameBytes)
	texture := strings.Repeat("t", validate.MaximumTexturePathBytes-len(".dds")) + ".dds"
	ref := func(plugin string) *pb.FormRef {
		return &pb.FormRef{Plugin: plugin, FormId: validate.MaximumLocalFormID}
	}
	color := &pb.Color{R: 0xFF, G: 0xFF, B: 0xFF}
	morphs := make([]float32, validate.FaceMorphCount)
	for i := range morphs {
		morphs[i] = math.MaxFloat32
	}
	parts := make([]int32, validate.FacePartCount)
	for i := range parts {
		parts[i] = validate.MaximumFacePart
	}
	a := &pb.Appearance{
		Revision: math.MaxInt64, Name: strings.Repeat("n", validate.MaximumNameBytes), Level: validate.MaximumLevel,
		Race: ref(plugin), Female: true, Weight: 100, Height: 4, HairColor: color, FaceDetails: ref(plugin),
		BodyTint: color, FaceMorphs: morphs, FaceParts: parts, DefaultRace: ref("ccfsvsse001-backpacks.esl"),
	}
	for range validate.MaximumHeadParts {
		a.HeadParts = append(a.HeadParts, &pb.HeadPart{Form: ref(plugin), Type: math.MaxUint32})
	}
	for range validate.MaximumTints {
		a.Tints = append(a.Tints, &pb.Tint{Index: validate.MaximumTintIndex, Type: validate.MaximumTintType,
			Color: color, Alpha: 1, Texture: texture})
	}
	for range validate.MaximumOverlays {
		a.Overlays = append(a.Overlays, &pb.Overlay{Area: pb.OverlayArea_OVERLAY_AREA_FACE, Texture: texture,
			Color: color, Alpha: 1, GlowColor: color, GlowStrength: validate.MaximumGlowStrength})
	}
	face := &pb.DefaultFace{Female: true, HairColor: color, FaceDetails: ref("ccfsvsse001-backpacks.esl"),
		BodyTint: color, FaceMorphs: morphs, FaceParts: parts}
	for headPartType := range uint32(validate.MaximumDefaultFaceHeadPartType + 1) {
		face.HeadParts = append(face.HeadParts, &pb.HeadPart{Form: ref("ccfsvsse001-backpacks.esl"), Type: headPartType})
	}
	for range validate.MaximumDefaultFaceTints {
		face.Tints = append(face.Tints, &pb.Tint{Index: validate.MaximumTintIndex, Type: validate.MaximumTintType,
			Color: color, Alpha: 1})
	}
	a.DefaultFace = face
	for i := range validate.MaximumBodyMorphs {
		name := fmt.Sprintf("%03d%s", i, strings.Repeat("~", validate.MaximumBodyMorphNameBytes-3))
		a.BodyMorphs = append(a.BodyMorphs, &pb.BodyMorph{Name: name, Value: -validate.MaximumBodyMorphValue})
	}
	for i := range validate.MaximumFaceSliders {
		name := fmt.Sprintf("%03d%s", i, strings.Repeat("~", validate.MaximumFaceSliderNameBytes-3))
		a.FaceSliders = append(a.FaceSliders, &pb.FaceSlider{Name: name, Value: -validate.MaximumFaceSliderValue})
	}
	return &pb.ClientMessage{Message: &pb.ClientMessage_Appearance{Appearance: a}}
}

func TestLargestAppearanceFitsTheReadLimit(t *testing.T) {
	m := largestAppearance()
	if err := validate.Message(m); err != nil {
		t.Fatal(err)
	}
	size := proto.Size(m)
	t.Logf("largest appearance: %d bytes, limit %d", size, readLimitBytes)
	if size > readLimitBytes {
		t.Fatalf("the largest valid appearance is %d bytes, over the %d-byte read limit", size, readLimitBytes)
	}
}
