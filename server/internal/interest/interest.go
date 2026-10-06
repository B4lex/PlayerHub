package interest

import (
	"math"
	"time"

	"playerhub/server/internal/wire/pb"
)

type Config struct {
	EnterRadius       float64
	LeaveRadius       float64
	MinDwellSeconds   float64
	UpdateHz          float64
	MaxVisible        int
	CapHysteresis     float64
	NearDistance      float64
	FarRateHz         float64
	MinimumViewRadius float64
}

func DefaultConfig() Config {
	return Config{EnterRadius: 8192, LeaveRadius: 9830, MinDwellSeconds: 1, UpdateHz: 5, MaxVisible: 64,
		CapHysteresis: 1024, NearDistance: 4096, FarRateHz: 10, MinimumViewRadius: 1024}
}

type Space struct {
	Worldspace uint32
	Cell       uint32
}

func SpaceOf(c *pb.Context) Space {
	if c.GetWorldspaceId() != 0 {
		return Space{Worldspace: c.GetWorldspaceId()}
	}
	return Space{Cell: c.GetCellId()}
}

type View struct {
	Slot     uint32
	Space    Space
	X, Y     float64
	Radius   float64
	Session  uint64
	Vanished map[uint64]bool
}

func Distance(a, b *View) float64 {
	return math.Hypot(a.X-b.X, a.Y-b.Y)
}

type Exclusion interface {
	Excludes(viewer, target *View) bool
}

type Verdict struct {
	Visible bool
	Reason  pb.LeaveReason
}

type Rules struct {
	config     Config
	exclusions []Exclusion
}

func NewRules(c Config, exclusions ...Exclusion) *Rules {
	return &Rules{config: c, exclusions: exclusions}
}

func (r *Rules) Config() Config { return r.config }

func (r *Rules) Decide(viewer, target *View, subscribedAt, now time.Time) Verdict {
	if viewer.Space != target.Space {
		return Verdict{Reason: pb.LeaveReason_LEAVE_REASON_OUT_OF_RANGE}
	}
	for _, rule := range r.exclusions {
		if rule.Excludes(viewer, target) {
			return Verdict{Reason: pb.LeaveReason_LEAVE_REASON_EXCLUDED}
		}
	}
	distance := Distance(viewer, target)
	enter, leave := r.Radii(viewer)
	if subscribedAt.IsZero() {
		return Verdict{Visible: distance <= enter}
	}
	if distance <= leave || now.Sub(subscribedAt).Seconds() < r.config.MinDwellSeconds {
		return Verdict{Visible: true}
	}
	return Verdict{Reason: pb.LeaveReason_LEAVE_REASON_OUT_OF_RANGE}
}

func (r *Rules) Radii(viewer *View) (enter, leave float64) {
	enter, leave = r.config.EnterRadius, r.config.LeaveRadius
	if viewer.Radius > 0 && viewer.Radius < enter {
		requested := max(viewer.Radius, r.config.MinimumViewRadius)
		leave *= requested / enter
		enter = requested
	}
	return enter, leave
}

func (r *Rules) Interval(distance float64) time.Duration {
	if r.config.FarRateHz <= 0 || distance <= r.config.NearDistance {
		return 0
	}
	return time.Duration(float64(time.Second) / r.config.FarRateHz)
}

type Hidden map[uint32]bool

func (h Hidden) Excludes(_, target *View) bool {
	return h[target.Slot]
}

type Pair struct {
	Viewer, Target uint32
}

type Pairs map[Pair]bool

func (p Pairs) Excludes(viewer, target *View) bool {
	return p[Pair{viewer.Slot, target.Slot}]
}

type Vanished struct{}

func (Vanished) Excludes(viewer, target *View) bool {
	return target.Session != 0 && viewer.Vanished[target.Session]
}
