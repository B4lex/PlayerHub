package loadtest

import (
	"crypto/rand"
	"encoding/hex"
	"errors"
	"log/slog"
	"math"
	"slices"
	"strconv"
	"sync"
	"time"

	"skyrim-mp/server/internal/hub"
	"skyrim-mp/server/internal/playback"
	"skyrim-mp/server/internal/recording"
	"skyrim-mp/server/internal/world"
)

var ErrNotFound = errors.New("load batch not found")

type Settings struct {
	Players       int     `json:"players"`
	Transport     string  `json:"transport"`
	Seed          uint64  `json:"seed"`
	Jitter        float64 `json:"jitter"`
	RateSpread    float64 `json:"rateSpread"`
	RampUpSeconds float64 `json:"rampUpSeconds"`
}

type BotState struct {
	Index      int     `json:"index"`
	Name       string  `json:"name"`
	PlayerID   *string `json:"playerId"`
	Status     string  `json:"status"`
	StartMs    int64   `json:"startMs"`
	Rate       float64 `json:"rate"`
	PositionMs int64   `json:"positionMs"`
	Error      *string `json:"error"`
}

type State struct {
	ID          string     `json:"id"`
	RecordingID string     `json:"recordingId"`
	Label       string     `json:"label"`
	PlayerName  string     `json:"playerName"`
	CreatedAt   string     `json:"createdAt"`
	DurationMs  int64      `json:"durationMs"`
	Status      string     `json:"status"`
	Settings    Settings   `json:"settings"`
	Bots        []BotState `json:"bots"`
}

type bot struct {
	plan     BotPlan
	name     string
	slot     uint32
	session  *playback.Session
	errorMsg string
}

type Batch struct {
	ID       string
	settings Settings
	r        *recording.Recording
	loop     *hub.Loop
	log      *slog.Logger
	onChange func()
	created  time.Time

	mu      sync.Mutex
	bots    []*bot
	playing bool
	stop    chan struct{}
	done    chan struct{}
}

func (b *Batch) Settings() Settings { return b.settings }

func (b *Batch) State() State {
	b.mu.Lock()
	defer b.mu.Unlock()
	h := b.r.Header
	state := State{
		ID: b.ID, RecordingID: h.ID, Label: h.Label, PlayerName: h.PlayerName,
		CreatedAt: b.created.UTC().Format(time.RFC3339), DurationMs: b.r.DurationMs(),
		Status: b.statusLocked(), Settings: b.settings, Bots: make([]BotState, 0, len(b.bots)),
	}
	for _, bot := range b.bots {
		s := BotState{Index: bot.plan.Index, Name: bot.name, StartMs: bot.plan.StartMs, Rate: bot.plan.Rate,
			PositionMs: bot.plan.StartMs, Status: "pending"}
		if bot.errorMsg != "" {
			s.Status, s.Error = "failed", &bot.errorMsg
		}
		if bot.session != nil {
			id := strconv.FormatUint(uint64(bot.slot), 10)
			s.PlayerID = &id
			s.PositionMs = int64(math.Round(bot.session.PositionMs()))
			s.Status = "paused"
			if bot.session.Status() == playback.StatusPlaying {
				s.Status = "playing"
			}
		}
		state.Bots = append(state.Bots, s)
	}
	return state
}

func (b *Batch) Slots() []uint32 {
	b.mu.Lock()
	defer b.mu.Unlock()
	var slots []uint32
	for _, bot := range b.bots {
		if bot.session != nil {
			slots = append(slots, bot.slot)
		}
	}
	return slots
}

func (b *Batch) statusLocked() string {
	if !b.playing {
		return "paused"
	}
	for _, bot := range b.bots {
		if bot.session == nil && bot.errorMsg == "" {
			return "starting"
		}
	}
	return "running"
}

func (b *Batch) Pause() {
	b.mu.Lock()
	b.playing = false
	for _, bot := range b.bots {
		if bot.session != nil {
			bot.session.Pause()
		}
	}
	b.mu.Unlock()
	b.onChange()
}

func (b *Batch) Resume() {
	b.mu.Lock()
	b.playing = true
	for _, bot := range b.bots {
		if bot.session != nil {
			bot.session.Play()
		}
	}
	b.mu.Unlock()
	b.onChange()
}

func (b *Batch) close() {
	close(b.stop)
	<-b.done
	b.mu.Lock()
	sessions := make([]*playback.Session, 0, len(b.bots))
	for _, bot := range b.bots {
		if bot.session != nil {
			sessions = append(sessions, bot.session)
		}
	}
	b.mu.Unlock()
	for _, s := range sessions {
		s.Close()
	}
}

func (b *Batch) rampUp() {
	defer close(b.done)
	started := time.Now()
	for _, bot := range b.bots {
		delay := time.Until(started.Add(time.Duration(bot.plan.JoinDelaySeconds * float64(time.Second))))
		if delay > 0 {
			select {
			case <-b.stop:
				return
			case <-time.After(delay):
			}
		}
		select {
		case <-b.stop:
			return
		default:
		}
		b.join(bot)
	}
	b.log.Info("load batch joined", "id", b.ID, "bots", len(b.bots))
}

func (b *Batch) join(bot *bot) {
	player, ok := playback.Spawn(b.loop, world.KindBot, b.log)
	if !ok {
		b.fail(bot, "server is stopping")
		return
	}
	b.mu.Lock()
	playing := b.playing
	b.mu.Unlock()
	session, err := playback.NewSession(b.ID+"-"+strconv.Itoa(bot.plan.Index+1), strconv.FormatUint(uint64(player.Slot()), 10),
		b.r, player, playback.Options{Autoplay: playing, Rate: bot.plan.Rate, Loop: true, StartMs: bot.plan.StartMs,
			Name: bot.name, OnChange: b.onChange})
	if err != nil {
		player.Leave()
		b.fail(bot, err.Error())
		return
	}
	b.mu.Lock()
	bot.slot, bot.session = player.Slot(), session
	b.mu.Unlock()
	session.Start()
	b.onChange()
}

func (b *Batch) fail(bot *bot, message string) {
	b.log.Warn("load-test bot could not join", "batch", b.ID, "bot", bot.plan.Index+1, "error", message)
	b.mu.Lock()
	bot.errorMsg = message
	b.mu.Unlock()
	b.onChange()
}

type Manager struct {
	store    *recording.Store
	loop     *hub.Loop
	log      *slog.Logger
	onChange func()

	mu      sync.Mutex
	batches []*Batch
}

func NewManager(store *recording.Store, loop *hub.Loop, log *slog.Logger, onChange func()) *Manager {
	return &Manager{store: store, loop: loop, log: log, onChange: onChange}
}

func (m *Manager) Create(recordingID string, settings Settings) (*Batch, error) {
	r, err := m.store.Load(recordingID)
	if err != nil {
		return nil, err
	}
	if r.DurationMs() <= 0 {
		return nil, errors.New("recording has no duration")
	}
	settings.Transport = "virtual"
	base := r.Header.PlayerName
	if base == "" {
		base = "Bot"
	}
	b := &Batch{
		ID: randomID(), settings: settings, r: r, loop: m.loop, log: m.log, onChange: m.onChange,
		created: time.Now(), playing: true, stop: make(chan struct{}), done: make(chan struct{}),
	}
	for _, plan := range PlanBots(settings.Players, r.DurationMs(), settings.Seed, settings.Jitter,
		settings.RateSpread, settings.RampUpSeconds) {
		b.bots = append(b.bots, &bot{plan: plan, name: playback.BotName(base, plan.Index)})
	}
	m.mu.Lock()
	m.batches = append(m.batches, b)
	m.mu.Unlock()
	go b.rampUp()
	m.log.Info("load batch started", "id", b.ID, "recording", recordingID, "bots", settings.Players)
	m.onChange()
	return b, nil
}

func (m *Manager) Get(id string) (*Batch, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	for _, b := range m.batches {
		if b.ID == id {
			return b, nil
		}
	}
	return nil, ErrNotFound
}

func (m *Manager) Batches() []*Batch {
	m.mu.Lock()
	defer m.mu.Unlock()
	return slices.Clone(m.batches)
}

func (m *Manager) Slots() map[uint32]bool {
	slots := make(map[uint32]bool)
	for _, b := range m.Batches() {
		for _, slot := range b.Slots() {
			slots[slot] = true
		}
	}
	return slots
}

func (m *Manager) Close(id string) error {
	m.mu.Lock()
	index := slices.IndexFunc(m.batches, func(b *Batch) bool { return b.ID == id })
	var b *Batch
	if index >= 0 {
		b = m.batches[index]
		m.batches = slices.Delete(m.batches, index, index+1)
	}
	m.mu.Unlock()
	if b == nil {
		return ErrNotFound
	}
	b.close()
	m.log.Info("load batch stopped", "id", id)
	m.onChange()
	return nil
}

func (m *Manager) CloseAll() {
	for _, b := range m.Batches() {
		m.Close(b.ID)
	}
}

func randomID() string {
	var b [4]byte
	rand.Read(b[:])
	return hex.EncodeToString(b[:])
}
