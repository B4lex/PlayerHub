// Package admin serves the admin panel: REST commands under /api, a
// server-sent event feed of state snapshots, and the built panel at /admin.
package admin

import (
	"strconv"
	"sync"
	"time"

	"skyrim-mp/server/internal/delivery"
	"skyrim-mp/server/internal/hub"
	"skyrim-mp/server/internal/loadtest"
	"skyrim-mp/server/internal/playback"
	"skyrim-mp/server/internal/recording"
	"skyrim-mp/server/internal/wire"
	"skyrim-mp/server/internal/world"
)

// Context is the part of the server the admin API reads and controls.
type Context struct {
	Loop      *hub.Loop
	Store     *recording.Store
	Captures  *recording.Captures
	Playbacks *playback.Manager
	Batches   *loadtest.Manager
	StartedAt time.Time
	// Tells open panels that state changed.
	OnChange func()

	rates rateTracker
}

// rateTracker turns each recipient's counters into per-second rates between
// snapshots, which the feed builds at most twice a second.
type rateTracker struct {
	mu      sync.Mutex
	samples map[uint32]rateSample
}

type rateSample struct {
	at                        time.Time
	written, replaced         uint64
	writtenRate, replacedRate float64
}

const minimumRateWindowSeconds = 0.25

func (t *rateTracker) rates(slot uint32, now time.Time, written, replaced uint64) (float64, float64) {
	t.mu.Lock()
	defer t.mu.Unlock()
	if t.samples == nil {
		t.samples = make(map[uint32]rateSample)
	}
	previous, ok := t.samples[slot]
	if !ok {
		t.samples[slot] = rateSample{at: now, written: written, replaced: replaced}
		return 0, 0
	}
	elapsed := now.Sub(previous.at).Seconds()
	if elapsed < minimumRateWindowSeconds {
		return previous.writtenRate, previous.replacedRate
	}
	next := rateSample{at: now, written: written, replaced: replaced,
		writtenRate:  float64(written-previous.written) / elapsed,
		replacedRate: float64(replaced-previous.replaced) / elapsed}
	t.samples[slot] = next
	return next.writtenRate, next.replacedRate
}

// forget drops samples of players that are gone.
func (t *rateTracker) forget(keep map[uint32]bool) {
	t.mu.Lock()
	defer t.mu.Unlock()
	for slot := range t.samples {
		if !keep[slot] {
			delete(t.samples, slot)
		}
	}
}

type Position struct {
	X float64 `json:"x"`
	Y float64 `json:"y"`
	Z float64 `json:"z"`
}

type Player struct {
	ID            string    `json:"id"`
	Name          string    `json:"name"`
	Virtual       bool      `json:"virtual"`
	InWorld       bool      `json:"inWorld"`
	ConnectedAtMs *float64  `json:"connectedAtMs"`
	CellID        *uint32   `json:"cellId"`
	WorldspaceID  *uint32   `json:"worldspaceId"`
	Position      *Position `json:"position"`
	MovementState *string   `json:"movementState"`
	// Peers this player receives.
	Visible int `json:"visible"`
	// Hidden from every other player by an admin.
	Hidden bool `json:"hidden"`
	// Bytes per second written to this client, and checkpoints per second
	// replaced by a newer one before they were written.
	BytesPerSecond    float64 `json:"bytesPerSecond"`
	ReplacedPerSecond float64 `json:"replacedPerSecond"`
	RecordingID       *string `json:"recordingId"`
	PlaybackID        *string `json:"playbackId"`
}

// Exclusion is a directed pair: the viewer never sees the target.
type Exclusion struct {
	ViewerID string `json:"viewerId"`
	TargetID string `json:"targetId"`
}

type Server struct {
	StartedAtMs  float64 `json:"startedAtMs"`
	ServerTimeMs float64 `json:"serverTimeMs"`
}

type State struct {
	Server     Server           `json:"server"`
	Players    []Player         `json:"players"`
	Recordings []recording.Info `json:"recordings"`
	Playbacks  []playback.State `json:"playbacks"`
	Batches    []loadtest.State `json:"batches"`
	Exclusions []Exclusion      `json:"exclusions"`
}

var movementStateNames = []string{"idle", "walking", "running", "sprinting", "sneakingIdle", "sneakingWalking", "sneakingRunning"}

func milliseconds(t time.Time) float64 {
	return float64(t.UnixNano()) / 1e6
}

func optional[T any](value T) *T {
	return &value
}

// players lists connected clients, and the excluded pairs. Replays and bots
// are shown by their playback or batch instead.
func (c *Context) players() ([]Player, []Exclusion) {
	var players []Player
	exclusions := []Exclusion{}
	now := time.Now()
	present := make(map[uint32]bool)
	c.Loop.Call(func(h *hub.Hub) {
		for _, pair := range h.Exclusions() {
			exclusions = append(exclusions, Exclusion{slotID(pair.Viewer), slotID(pair.Target)})
		}
		h.Players(func(p *world.Player, visible int, outbox *delivery.Outbox) {
			if p.Kind != world.KindClient {
				return
			}
			present[p.Slot] = true
			name := h.Name(p.Slot)
			player := Player{
				ID: slotID(p.Slot), Name: name, InWorld: p.InWorld,
				ConnectedAtMs: optional(milliseconds(p.ConnectedAt)), Visible: visible, Hidden: h.Hidden(p.Slot),
			}
			if outbox != nil {
				written, replaced := outbox.Counters()
				player.BytesPerSecond, player.ReplacedPerSecond = c.rates.rates(p.Slot, now, written, replaced)
			}
			if p.Context != nil {
				player.CellID, player.WorldspaceID = optional(p.Context.CellId), optional(p.Context.WorldspaceId)
			}
			if c := p.Checkpoint; c != nil {
				player.Position = &Position{float64(c.X), float64(c.Y), float64(c.Z)}
				player.MovementState = optional(movementStateNames[wire.MovementState(c)])
			}
			players = append(players, player)
		})
	})
	c.rates.forget(present)
	for i := range players {
		slot, _ := strconv.ParseUint(players[i].ID, 10, 32)
		if id := c.Captures.RecordingFor(uint32(slot)); id != "" {
			players[i].RecordingID = &id
		}
	}
	return players, exclusions
}

func slotID(slot uint32) string {
	return strconv.FormatUint(uint64(slot), 10)
}

func (c *Context) recordings() []recording.Info {
	live := c.Captures.ActiveInfos()
	skip := make(map[string]bool, len(live))
	for _, info := range live {
		skip[info.ID] = true
	}
	infos := append(c.Store.Infos(skip), live...)
	recording.SortNewestFirst(infos)
	return infos
}

func (c *Context) State() State {
	players, exclusions := c.players()
	state := State{
		Server:     Server{StartedAtMs: milliseconds(c.StartedAt), ServerTimeMs: milliseconds(time.Now())},
		Players:    players,
		Recordings: c.recordings(),
		Playbacks:  []playback.State{},
		Batches:    []loadtest.State{},
		Exclusions: exclusions,
	}
	for _, s := range c.Playbacks.Sessions() {
		playbackState := s.State()
		state.Playbacks = append(state.Playbacks, playbackState)
		state.Players = append(state.Players, Player{
			ID: playbackState.PlayerID, Name: playbackState.PlayerName, Virtual: true, InWorld: s.InWorld(),
			PlaybackID: optional(playbackState.ID),
		})
	}
	for _, b := range c.Batches.Batches() {
		state.Batches = append(state.Batches, b.State())
	}
	if state.Players == nil {
		state.Players = []Player{}
	}
	if state.Recordings == nil {
		state.Recordings = []recording.Info{}
	}
	return state
}
