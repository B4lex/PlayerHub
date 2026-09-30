// Package admin serves the admin panel: REST commands under /api, a
// server-sent event feed of state snapshots, and the built panel at /admin.
package admin

import (
	"strconv"
	"time"

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
	Visible     int     `json:"visible"`
	RecordingID *string `json:"recordingId"`
	PlaybackID  *string `json:"playbackId"`
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
}

var movementStateNames = []string{"idle", "walking", "running", "sprinting", "sneakingIdle", "sneakingWalking", "sneakingRunning"}

func milliseconds(t time.Time) float64 {
	return float64(t.UnixNano()) / 1e6
}

func optional[T any](value T) *T {
	return &value
}

// players lists connected clients. Replays and bots are shown by their
// playback or batch instead.
func (c *Context) players() []Player {
	var players []Player
	c.Loop.Call(func(h *hub.Hub) {
		h.Players(func(p *world.Player, visible int) {
			if p.Kind != world.KindClient {
				return
			}
			name, _ := h.Name(p.Slot)
			player := Player{
				ID: strconv.FormatUint(uint64(p.Slot), 10), Name: name, InWorld: p.InWorld,
				ConnectedAtMs: optional(milliseconds(p.ConnectedAt)), Visible: visible,
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
	for i := range players {
		slot, _ := strconv.ParseUint(players[i].ID, 10, 32)
		if id := c.Captures.RecordingFor(uint32(slot)); id != "" {
			players[i].RecordingID = &id
		}
	}
	return players
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
	state := State{
		Server:     Server{StartedAtMs: milliseconds(c.StartedAt), ServerTimeMs: milliseconds(time.Now())},
		Players:    c.players(),
		Recordings: c.recordings(),
		Playbacks:  []playback.State{},
		Batches:    []loadtest.State{},
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
