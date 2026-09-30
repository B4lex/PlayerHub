package playback

import (
	"errors"
	"fmt"
	"math"
	"sort"
	"sync"
	"time"

	"skyrim-mp/server/internal/recording"
	"skyrim-mp/server/internal/wire/pb"
)

const (
	MinimumRate = 0.1
	MaximumRate = 4.0
	// Timeline positions come from float clock arithmetic, so a message due at
	// exactly the current position can appear a hair in the future.
	dueToleranceMs = 1e-6
)

var ErrNoCheckpoints = errors.New("recording has no checkpoints")

// Player is where a replay's messages go.
type Player interface {
	Send(m *pb.ClientMessage)
	// Leave removes the player from every client.
	Leave()
}

type Status string

const (
	StatusPlaying Status = "playing"
	StatusPaused  Status = "paused"
	StatusEnded   Status = "ended"
)

type Options struct {
	Autoplay bool
	Rate     float64
	Loop     bool
	StartMs  int64
	// Replaces the recorded player name, for load-test bots.
	Name     string
	Now      func() time.Time
	OnChange func()
}

// State is a replay as the admin panel shows it.
type State struct {
	ID          string  `json:"id"`
	RecordingID string  `json:"recordingId"`
	Label       string  `json:"label"`
	PlayerID    string  `json:"playerId"`
	PlayerName  string  `json:"playerName"`
	Status      Status  `json:"status"`
	PositionMs  int64   `json:"positionMs"`
	DurationMs  int64   `json:"durationMs"`
	Rate        float64 `json:"rate"`
	Loop        bool    `json:"loop"`
	// Rises on every change, so a newer copy supersedes an older one.
	Revision int64   `json:"revision"`
	Error    *string `json:"error"`
}

type timed[T any] struct {
	tMs   int64
	value T
}

// Session replays one recording. The timeline position is anchorMs at
// anchorTime, moving at rate while playing. Control methods only change that
// state and wake the run goroutine, which emits every message, so a seek takes
// effect between two emissions. Checkpoints are latest-wins, so a seek does
// not replay what it skips: it sends the state in effect at the target. While
// the recorded player was out of the world, the virtual player is too.
type Session struct {
	ID       string
	PlayerID string

	mu          sync.Mutex
	recording   *recording.Recording
	player      Player
	virtual     VirtualPlayer
	now         func() time.Time
	onChange    func()
	times       []int64
	checkpoints []timed[*pb.Checkpoint]
	contexts    []timed[*pb.Context]
	appearances []timed[*pb.Appearance]
	equipment   []timed[*pb.Equipment]
	presence    []timed[bool]
	duration    int64

	playing         bool
	ended           bool
	rate            float64
	loop            bool
	anchorMs        float64
	anchorTime      time.Time
	cursor          int
	snapshotPending bool
	holdPending     bool
	lastCheckpoint  *pb.Checkpoint
	inWorld         bool
	contextSent     bool
	revision        int64

	wake chan struct{}
	stop chan struct{}
	done chan struct{}
}

func NewSession(id, playerID string, r *recording.Recording, player Player, o Options) (*Session, error) {
	if err := ValidateRate(o.Rate); err != nil {
		return nil, err
	}
	s := &Session{
		ID: id, PlayerID: playerID, recording: r, player: player,
		virtual: VirtualPlayer{Name: o.Name}, now: o.Now, onChange: o.OnChange,
		duration: r.DurationMs(), playing: o.Autoplay, rate: o.Rate, loop: o.Loop,
		snapshotPending: true,
		wake:            make(chan struct{}, 1), stop: make(chan struct{}), done: make(chan struct{}),
	}
	if s.now == nil {
		s.now = time.Now
	}
	if s.onChange == nil {
		s.onChange = func() {}
	}
	for _, m := range r.Messages {
		s.times = append(s.times, m.TMs)
		switch body := m.Message.Message.(type) {
		case *pb.ClientMessage_Checkpoint:
			s.checkpoints = append(s.checkpoints, timed[*pb.Checkpoint]{m.TMs, body.Checkpoint})
		case *pb.ClientMessage_Context:
			s.contexts = append(s.contexts, timed[*pb.Context]{m.TMs, body.Context})
		case *pb.ClientMessage_Appearance:
			s.appearances = append(s.appearances, timed[*pb.Appearance]{m.TMs, body.Appearance})
		case *pb.ClientMessage_Equipment:
			s.equipment = append(s.equipment, timed[*pb.Equipment]{m.TMs, body.Equipment})
		case *pb.ClientMessage_Enter:
			s.presence = append(s.presence, timed[bool]{m.TMs, true})
		case *pb.ClientMessage_Exit:
			s.presence = append(s.presence, timed[bool]{m.TMs, false})
		}
	}
	if len(s.checkpoints) == 0 || len(s.contexts) == 0 {
		return nil, ErrNoCheckpoints
	}
	s.anchorMs = math.Min(math.Max(float64(o.StartMs), 0), float64(s.duration))
	s.anchorTime = s.now()
	return s, nil
}

func ValidateRate(rate float64) error {
	if !(rate >= MinimumRate && rate <= MaximumRate) {
		return fmt.Errorf("rate must be between %g and %g", MinimumRate, MaximumRate)
	}
	return nil
}

// Start runs the emission goroutine.
func (s *Session) Start() {
	go s.run()
}

// Close stops the replay and removes its player from every client.
func (s *Session) Close() {
	select {
	case <-s.stop:
	default:
		close(s.stop)
	}
	<-s.done
	s.player.Leave()
}

func (s *Session) State() State {
	s.mu.Lock()
	defer s.mu.Unlock()
	h := s.recording.Header
	return State{
		ID: s.ID, RecordingID: h.ID, Label: h.Label, PlayerID: s.PlayerID, PlayerName: h.PlayerName,
		Status: s.statusLocked(), PositionMs: int64(math.Round(s.positionLocked(s.now()))), DurationMs: s.duration,
		Rate: s.rate, Loop: s.loop, Revision: s.revision,
	}
}

func (s *Session) InWorld() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.inWorld
}

func (s *Session) Status() Status {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.statusLocked()
}

func (s *Session) PositionMs() float64 {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.positionLocked(s.now())
}

func (s *Session) Play() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.playing {
		return
	}
	if s.ended || s.anchorMs >= float64(s.duration) {
		// Like a media player, playing from the end starts over.
		s.anchorMs = 0
		s.snapshotPending = true
	}
	s.ended = false
	s.playing = true
	s.anchorTime = s.now()
	s.touchLocked()
}

func (s *Session) Pause() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.playing {
		return
	}
	s.anchorMs = s.positionLocked(s.now())
	s.playing = false
	s.holdPending = true
	s.touchLocked()
}

func (s *Session) Seek(positionMs float64) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.anchorMs = math.Min(math.Max(positionMs, 0), float64(s.duration))
	s.anchorTime = s.now()
	s.ended = false
	s.snapshotPending = true
	s.touchLocked()
}

// Configure changes the rate, the looping, or both.
func (s *Session) Configure(rate *float64, loop *bool) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	if rate != nil {
		if err := ValidateRate(*rate); err != nil {
			return err
		}
		now := s.now()
		s.anchorMs = s.positionLocked(now)
		s.anchorTime = now
		s.rate = *rate
	}
	if loop != nil {
		s.loop = *loop
	}
	s.touchLocked()
	return nil
}

// Advance emits whatever is due and returns how long until more is due, or
// a negative duration when nothing is scheduled.
func (s *Session) Advance() time.Duration {
	s.mu.Lock()
	defer s.mu.Unlock()
	now := s.now()
	if s.snapshotPending {
		s.snapshotPending = false
		position := s.positionLocked(now)
		s.cursor = sort.Search(len(s.times), func(i int) bool { return float64(s.times[i]) > position })
		s.emitSnapshotLocked(position)
	}
	if s.playing {
		due := s.positionLocked(now) + dueToleranceMs
		messages := s.recording.Messages
		for s.cursor < len(messages) && float64(messages[s.cursor].TMs) <= due {
			s.emitLocked(messages[s.cursor])
			s.cursor++
		}
		if s.cursor >= len(messages) && due >= float64(s.duration) {
			s.reachEndLocked(now)
		}
	} else if s.holdPending {
		s.holdPending = false
		if s.lastCheckpoint != nil && s.inWorld {
			s.player.Send(checkpointMessage(s.virtual.Checkpoint(s.lastCheckpoint, true)))
		}
	}
	return s.nextDelayLocked(now)
}

func (s *Session) run() {
	defer close(s.done)
	timer := time.NewTimer(time.Hour)
	defer timer.Stop()
	for {
		delay := s.Advance()
		var fire <-chan time.Time
		if delay >= 0 {
			timer.Reset(delay)
			fire = timer.C
		}
		select {
		case <-s.stop:
			return
		case <-s.wake:
		case <-fire:
		}
		if !timer.Stop() {
			select {
			case <-timer.C:
			default:
			}
		}
	}
}

func (s *Session) nextDelayLocked(now time.Time) time.Duration {
	if s.snapshotPending || s.holdPending {
		return 0
	}
	if !s.playing {
		return -1
	}
	due := s.duration
	if s.cursor < len(s.recording.Messages) {
		due = s.recording.Messages[s.cursor].TMs
	}
	remainingMs := math.Max(0, (float64(due)-s.positionLocked(now))/s.rate)
	return time.Duration(remainingMs * float64(time.Millisecond))
}

func (s *Session) emitSnapshotLocked(position float64) {
	s.virtual.BreakContinuity()
	if a := latestAt(s.appearances, position); a != nil {
		if numbered := s.virtual.Appearance(a); numbered != nil {
			s.player.Send(&pb.ClientMessage{Message: &pb.ClientMessage_Appearance{Appearance: numbered}})
		}
	}
	if e := latestAt(s.equipment, position); e != nil {
		if numbered := s.virtual.Equipment(e); numbered != nil {
			s.player.Send(&pb.ClientMessage{Message: &pb.ClientMessage_Equipment{Equipment: numbered}})
		}
	}
	checkpoint := s.checkpointAtLocked(position)
	if checkpoint == nil {
		s.exitWorldLocked()
		return
	}
	s.enterWorldLocked()
	// Always sent after a break, to start the new graph epoch.
	s.sendContextLocked(latestAt(s.contexts, position))
	s.lastCheckpoint = checkpoint
	s.player.Send(checkpointMessage(s.virtual.Checkpoint(checkpoint, !s.playing)))
}

// checkpointAtLocked returns the checkpoint in effect, or nil while the
// player is out of the world.
func (s *Session) checkpointAtLocked(position float64) *pb.Checkpoint {
	presence := sort.Search(len(s.presence), func(i int) bool { return float64(s.presence[i].tMs) > position }) - 1
	if presence < 0 {
		// Before any recorded change, the player is in the world unless it enters later.
		if len(s.presence) > 0 && s.presence[0].value {
			return nil
		}
		return latestAt(s.checkpoints, position)
	}
	if !s.presence[presence].value {
		return nil
	}
	index := sort.Search(len(s.checkpoints), func(i int) bool { return float64(s.checkpoints[i].tMs) > position }) - 1
	if index >= 0 && s.checkpoints[index].tMs < s.presence[presence].tMs {
		// The latest checkpoint predates this entry; wait for the next one.
		return nil
	}
	return latestAt(s.checkpoints, position)
}

func (s *Session) emitLocked(m recording.Message) {
	switch body := m.Message.Message.(type) {
	case *pb.ClientMessage_Exit:
		s.exitWorldLocked()
	case *pb.ClientMessage_Context:
		s.enterWorldLocked()
		s.sendContextLocked(body.Context)
	case *pb.ClientMessage_Checkpoint:
		s.enterWorldLocked()
		if !s.contextSent {
			s.sendContextLocked(latestAt(s.contexts, float64(m.TMs)))
		}
		s.lastCheckpoint = body.Checkpoint
		s.player.Send(checkpointMessage(s.virtual.Checkpoint(body.Checkpoint, false)))
	case *pb.ClientMessage_Appearance:
		if numbered := s.virtual.Appearance(body.Appearance); numbered != nil {
			s.player.Send(&pb.ClientMessage{Message: &pb.ClientMessage_Appearance{Appearance: numbered}})
		}
	case *pb.ClientMessage_Equipment:
		if numbered := s.virtual.Equipment(body.Equipment); numbered != nil {
			s.player.Send(&pb.ClientMessage{Message: &pb.ClientMessage_Equipment{Equipment: numbered}})
		}
	case *pb.ClientMessage_Action:
		if s.inWorld && s.lastCheckpoint != nil {
			if numbered := s.virtual.Action(body.Action); numbered != nil {
				s.player.Send(&pb.ClientMessage{Message: &pb.ClientMessage_Action{Action: numbered}})
			}
		}
	}
	// A recorded Enter places nothing by itself; the next context or
	// checkpoint enters the world.
}

func (s *Session) sendContextLocked(c *pb.Context) {
	s.player.Send(&pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: s.virtual.Context(c)}})
	s.contextSent = true
}

func (s *Session) enterWorldLocked() {
	if s.inWorld {
		return
	}
	s.inWorld = true
	s.contextSent = false
	s.player.Send(&pb.ClientMessage{Message: &pb.ClientMessage_Enter{Enter: &pb.Enter{}}})
	s.onChange()
}

func (s *Session) exitWorldLocked() {
	s.lastCheckpoint = nil
	if !s.inWorld {
		return
	}
	s.inWorld = false
	s.contextSent = false
	s.player.Send(&pb.ClientMessage{Message: &pb.ClientMessage_Exit{Exit: &pb.Exit{}}})
	s.onChange()
}

func (s *Session) reachEndLocked(now time.Time) {
	if s.loop && s.duration > 0 {
		s.anchorMs = 0
		s.anchorTime = now
		s.snapshotPending = true
	} else {
		s.anchorMs = float64(s.duration)
		s.playing = false
		s.ended = true
		s.holdPending = true
	}
	s.touchLocked()
}

func (s *Session) statusLocked() Status {
	switch {
	case s.playing:
		return StatusPlaying
	case s.ended:
		return StatusEnded
	}
	return StatusPaused
}

func (s *Session) positionLocked(now time.Time) float64 {
	if !s.playing {
		return s.anchorMs
	}
	elapsedMs := float64(now.Sub(s.anchorTime)) / float64(time.Millisecond) * s.rate
	return math.Min(float64(s.duration), s.anchorMs+elapsedMs)
}

func (s *Session) touchLocked() {
	s.revision++
	select {
	case s.wake <- struct{}{}:
	default:
	}
	s.onChange()
}

// latestAt returns the value in effect at position; before the first one, the
// first is the best estimate.
func latestAt[T any](series []timed[T], position float64) T {
	var zero T
	if len(series) == 0 {
		return zero
	}
	index := sort.Search(len(series), func(i int) bool { return float64(series[i].tMs) > position }) - 1
	return series[max(index, 0)].value
}

func checkpointMessage(c *pb.Checkpoint) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: c}}
}
