// Package loadtest runs batches of in-server bots that replay one recording
// from shifted positions.
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

// PlanBots spreads count bots over the recording. Bot i sits at
// i × duration / count, moved by up to jitter / 2 of that spacing either way,
// so neighbours never swap places. rateSpread varies each bot's speed around
// 1x so the spacing drifts instead of repeating each loop. A bot joining
// during the ramp-up starts that much further along, keeping the spacing.
// Offsets and rates use separate generators, so changing one setting keeps
// the other's layout for the same seed.
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
