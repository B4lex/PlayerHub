package interest

import (
	"slices"
	"testing"
	"time"

	"playerhub/server/internal/wire/pb"
)

func TestDecideHysteresisAndDwell(t *testing.T) {
	rules := NewRules(DefaultConfig())
	now := time.Unix(100, 0)
	viewer := &View{Slot: 1, Space: Space{FormID: 60}}
	at := func(x float64) *View { return &View{Slot: 2, Space: Space{FormID: 60}, X: x} }

	if !rules.Decide(viewer, at(8000), time.Time{}, now).Visible {
		t.Fatal("inside the enter radius was not entered")
	}
	if rules.Decide(viewer, at(9000), time.Time{}, now).Visible {
		t.Fatal("between the radii was entered")
	}
	long := now.Add(-2 * time.Second)
	if !rules.Decide(viewer, at(9000), long, now).Visible {
		t.Fatal("between the radii left")
	}
	if v := rules.Decide(viewer, at(10000), long, now); v.Visible || v.Reason != pb.LeaveReason_LEAVE_REASON_OUT_OF_RANGE {
		t.Fatalf("beyond the leave radius: %+v", v)
	}
	recent := now.Add(-500 * time.Millisecond)
	if !rules.Decide(viewer, at(20000), recent, now).Visible {
		t.Fatal("left before the minimum time in view")
	}
}

func TestSpaceAndExclusionsAreHard(t *testing.T) {
	hidden := Hidden{}
	pairs := Pairs{}
	rules := NewRules(DefaultConfig(), hidden, pairs)
	now := time.Unix(100, 0)
	recent := now.Add(-100 * time.Millisecond)
	viewer := &View{Slot: 1, Space: Space{FormID: 60}}
	target := &View{Slot: 2, Space: Space{FormID: 60}}

	interior := &View{Slot: 2, Space: Space{Interior: true, FormID: 7}}
	if v := rules.Decide(viewer, interior, recent, now); v.Visible {
		t.Fatal("another space stayed in view during the dwell")
	}
	hidden[2] = true
	if v := rules.Decide(viewer, target, recent, now); v.Visible || v.Reason != pb.LeaveReason_LEAVE_REASON_EXCLUDED {
		t.Fatalf("hidden: %+v", v)
	}
	if !rules.Decide(target, viewer, time.Time{}, now).Visible {
		t.Fatal("a hidden player stopped seeing others")
	}
	delete(hidden, 2)
	pairs[Pair{1, 2}] = true
	if rules.Decide(viewer, target, time.Time{}, now).Visible {
		t.Fatal("excluded pair entered")
	}
	if !rules.Decide(target, viewer, time.Time{}, now).Visible {
		t.Fatal("pair exclusion is not directed")
	}
}

func TestSpaceOf(t *testing.T) {
	ref := func(plugin string, id uint32) *pb.FormRef { return &pb.FormRef{Plugin: plugin, FormId: id} }
	if SpaceOf(&pb.Context{Worldspace: ref("Skyrim.esm", 60)}) != (Space{Plugin: "skyrim.esm", FormID: 60}) {
		t.Fatal("worldspace space differs")
	}
	if SpaceOf(&pb.Context{Worldspace: ref("Skyrim.esm", 60)}) != SpaceOf(&pb.Context{Worldspace: ref("SKYRIM.ESM", 60)}) {
		t.Fatal("plugin case separates a worldspace")
	}
	if SpaceOf(&pb.Context{InteriorCell: ref("Skyrim.esm", 5)}) == SpaceOf(&pb.Context{InteriorCell: ref("Skyrim.esm", 6)}) {
		t.Fatal("interiors compare equal")
	}
	if SpaceOf(&pb.Context{InteriorCell: ref("A.esp", 0x800)}) == SpaceOf(&pb.Context{InteriorCell: ref("B.esp", 0x800)}) {
		t.Fatal("interiors of different plugins compare equal")
	}
	if SpaceOf(&pb.Context{InteriorCell: ref("Skyrim.esm", 60)}) == SpaceOf(&pb.Context{Worldspace: ref("Skyrim.esm", 60)}) {
		t.Fatal("an interior compares equal to a worldspace")
	}
}

func TestGridFindsNeighboursOnly(t *testing.T) {
	g := NewGrid(10000)
	views := []*View{
		{Slot: 1, Space: Space{FormID: 60}, X: 0, Y: 0},
		{Slot: 2, Space: Space{FormID: 60}, X: 9999, Y: -9999},
		{Slot: 3, Space: Space{FormID: 60}, X: 25000, Y: 0},
		{Slot: 4, Space: Space{FormID: 61}, X: 0, Y: 0},
	}
	for _, v := range views {
		g.Update(v)
	}
	var near []uint32
	g.Near(views[0], func(slot uint32) { near = append(near, slot) })
	slices.Sort(near)
	if !slices.Equal(near, []uint32{1, 2}) {
		t.Fatalf("near %v", near)
	}
	views[2].X = 15000
	g.Update(views[2])
	g.Remove(2)
	near = near[:0]
	g.Near(views[0], func(slot uint32) { near = append(near, slot) })
	slices.Sort(near)
	if !slices.Equal(near, []uint32{1, 3}) {
		t.Fatalf("after moves %v", near)
	}
}

func TestViewerRadiusAndRateClasses(t *testing.T) {
	rules := NewRules(DefaultConfig())
	now := time.Unix(100, 0)
	viewer := &View{Slot: 1, Space: Space{FormID: 60}, Radius: 2048}
	target := &View{Slot: 2, Space: Space{FormID: 60}, X: 3000}
	if rules.Decide(viewer, target, time.Time{}, now).Visible {
		t.Fatal("entered beyond the viewer's own radius")
	}
	if enter, leave := rules.Radii(&View{Radius: 100}); enter != 1024 || leave != 9830.0/8192*1024 {
		t.Fatalf("radius not clamped: %v %v", enter, leave)
	}
	if enter, _ := rules.Radii(&View{Radius: 50000}); enter != 8192 {
		t.Fatalf("radius above the server's: %v", enter)
	}
	if rules.Interval(4000) != 0 || rules.Interval(5000) != 100*time.Millisecond {
		t.Fatal("rate classes")
	}
}
