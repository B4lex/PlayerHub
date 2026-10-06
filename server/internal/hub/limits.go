package hub

import "time"

const (
	chatMessagesPerWindow = 5
	chatWindowSeconds     = 5.0

	inspectsPerSecond = 2.0
	inspectBurst      = 5.0
)

type chatThrottle struct {
	accepted []time.Time
}

func (c *chatThrottle) allow(now time.Time) bool {
	cutoff := now.Add(-time.Duration(chatWindowSeconds * float64(time.Second)))
	kept := c.accepted[:0]
	for _, t := range c.accepted {
		if t.After(cutoff) {
			kept = append(kept, t)
		}
	}
	c.accepted = kept
	if len(c.accepted) >= chatMessagesPerWindow {
		return false
	}
	c.accepted = append(c.accepted, now)
	return true
}
