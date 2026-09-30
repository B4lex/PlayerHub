package loadtest

import (
	"slices"
	"testing"
)

func TestPlanSpreadsBotsWithoutSwapping(t *testing.T) {
	plans := PlanBots(10, 10000, 42, 1, 0, 0)
	for i, plan := range plans {
		low, high := int64(i*1000-500), int64(i*1000+500)
		if i == 0 {
			// The first bot may wrap to the end.
			continue
		}
		if plan.StartMs < low || plan.StartMs > high || plan.Rate != 1 || plan.JoinDelaySeconds != 0 {
			t.Fatalf("bot %d: %+v", i, plan)
		}
	}
	if !slices.Equal(plans, PlanBots(10, 10000, 42, 1, 0, 0)) {
		t.Fatal("same seed gave another layout")
	}
}

func TestRateSpreadKeepsOffsets(t *testing.T) {
	still := PlanBots(5, 10000, 7, 0.5, 0, 0)
	spread := PlanBots(5, 10000, 7, 0.5, 0.5, 0)
	for i := range still {
		if still[i].StartMs != spread[i].StartMs {
			t.Fatalf("rate spread moved bot %d", i)
		}
		if spread[i].Rate < 0.5 || spread[i].Rate > 1.5 {
			t.Fatalf("rate %v out of range", spread[i].Rate)
		}
	}
}

func TestRampUpKeepsSpacing(t *testing.T) {
	plans := PlanBots(4, 100000, 1, 0, 0, 20)
	for i, plan := range plans {
		want := int64(i*25000) + int64(plan.JoinDelaySeconds*1000)
		if plan.JoinDelaySeconds != float64(i)*5 || plan.StartMs != want {
			t.Fatalf("bot %d: %+v, want start %d", i, plan, want)
		}
	}
}
