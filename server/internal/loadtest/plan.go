package loadtest

import (
	"math"
	"math/rand/v2"

	"skyrim-mp/server/internal/playback"
)

const (
	MaximumBots          = 500
	MaximumRampUpSeconds = 600.0
	MaximumRateSpread    = 0.5
	offsetStream         = 1
	rateStream           = 2
)

type BotPlan struct {
	Index            int
	StartMs          int64
	Rate             float64
	JoinDelaySeconds float64
}

func PlanBots(count int, durationMs int64, seed uint64, jitter, rateSpread, rampUpSeconds float64) []BotPlan {
	offsets := rand.New(rand.NewPCG(seed, offsetStream))
	rates := rand.New(rand.NewPCG(seed, rateStream))
	slotMs := float64(durationMs) / float64(count)
	plans := make([]BotPlan, count)
	for i := range count {
		rate := 1 + (rates.Float64()*2-1)*rateSpread
		rate = math.Round(min(max(rate, playback.MinimumRate), playback.MaximumRate)*1000) / 1000
		joinDelay := rampUpSeconds * float64(i) / float64(count)
		offsetMs := (float64(i) + (offsets.Float64()-0.5)*jitter) * slotMs
		startMs := offsetMs + joinDelay*1000*rate
		var start int64
		if durationMs > 0 {
			start = int64(math.Mod(math.Mod(startMs, float64(durationMs))+float64(durationMs), float64(durationMs)))
		}
		plans[i] = BotPlan{Index: i, StartMs: start, Rate: rate, JoinDelaySeconds: joinDelay}
	}
	return plans
}
