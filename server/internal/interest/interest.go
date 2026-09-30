// Package interest decides which placed peers each viewer receives. It has no
// I/O and no clock of its own: the hub passes views and times, applies the
// decisions, and sends the resulting PeerEnter and PeerLeave messages.
package interest

import (
	"math"
	"time"

	"skyrim-mp/server/internal/wire/pb"
)

type Config struct {
	// A peer enters a viewer's view within EnterRadius and leaves it beyond
	// LeaveRadius, so it does not flap at the edge.
	EnterRadius float64
	LeaveRadius float64
	// A peer stays in view at least this long before leaving for range.
	MinDwellSeconds float64
	// How often the periodic pass reevaluates every pair.
	UpdateHz float64
	// Peers a viewer receives at most, nearest first; zero is no cap.
	MaxVisible int
	// A peer already in view keeps its place under the cap unless a newcomer
	// is this much nearer.
	CapHysteresis float64
	// Peers farther than NearDistance get checkpoints at most FarRateHz; zero
	// sends every checkpoint.
	NearDistance float64
	FarRateHz    float64
	// Clients may ask for a smaller radius, down to this.
	MinimumViewRadius float64
}

func DefaultConfig() Config {
	return Config{EnterRadius: 8192, LeaveRadius: 9830, MinDwellSeconds: 1, UpdateHz: 5, MaxVisible: 64,
		CapHysteresis: 1024, NearDistance: 4096, FarRateHz: 10, MinimumViewRadius: 1024}
}

// Space is a coordinate space. Exteriors of one worldspace share
// coordinates; an interior's are local to its cell.
type Space struct {
	Worldspace uint32
	// Only for an interior (Worldspace zero).
	Cell uint32
}

func SpaceOf(c *pb.Context) Space {
	if c.GetWorldspaceId() != 0 {
		return Space{Worldspace: c.GetWorldspaceId()}
	}
	return Space{Cell: c.GetCellId()}
}

// View is what the rules know about a placed player.
type View struct {
	Slot  uint32
	Space Space
	X, Y  float64
	// The enter radius this player asked for as a viewer; zero is the
	// server's.
	Radius float64
}

// Distance is horizontal: Skyrim loads cells as a horizontal grid, so height
// does not decide whether a peer can be shown.
func Distance(a, b *View) float64 {
	return math.Hypot(a.X-b.X, a.Y-b.Y)
}

// Exclusion is a hard rule: it removes the target from everything the viewer
// receives, at once, whatever the distance or time in view.
type Exclusion interface {
	Excludes(viewer, target *View) bool
}

// Verdict is the outcome for one (viewer, target) pair.
type Verdict struct {
	Visible bool
	// Why a subscribed target must leave now; only when !Visible.
	Reason pb.LeaveReason
}

// Rules evaluates pairs.
type Rules struct {
	config     Config
	exclusions []Exclusion
}

func NewRules(c Config, exclusions ...Exclusion) *Rules {
	return &Rules{config: c, exclusions: exclusions}
}

func (r *Rules) Config() Config { return r.config }

// Decide returns whether target should be in viewer's view. subscribedAt is
// when the pair's subscription began, or the zero time when it has none.
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

// Radii returns the viewer's enter and leave radius: its own request clamped
// to the server's, with the server's ratio between them.
func (r *Rules) Radii(viewer *View) (enter, leave float64) {
	enter, leave = r.config.EnterRadius, r.config.LeaveRadius
	if viewer.Radius > 0 && viewer.Radius < enter {
		requested := max(viewer.Radius, r.config.MinimumViewRadius)
		leave *= requested / enter
		enter = requested
	}
	return enter, leave
}

// Interval is the least time between checkpoints of a target at distance.
func (r *Rules) Interval(distance float64) time.Duration {
	if r.config.FarRateHz <= 0 || distance <= r.config.NearDistance {
		return 0
	}
	return time.Duration(float64(time.Second) / r.config.FarRateHz)
}

// Hidden is the admin rule that hides players from everyone: spectators and
// invisible admins. They still see others.
type Hidden map[uint32]bool

func (h Hidden) Excludes(_, target *View) bool {
	return h[target.Slot]
}

// Pair identifies one directed exclusion: Viewer never sees Target.
type Pair struct {
	Viewer, Target uint32
}

// Pairs is the admin rule that excludes single directed pairs.
type Pairs map[Pair]bool

func (p Pairs) Excludes(viewer, target *View) bool {
	return p[Pair{viewer.Slot, target.Slot}]
}
