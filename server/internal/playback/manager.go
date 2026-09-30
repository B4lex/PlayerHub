package playback

import (
	"crypto/rand"
	"encoding/hex"
	"errors"
	"log/slog"
	"slices"
	"strconv"
	"sync"
	"unicode/utf8"

	"skyrim-mp/server/internal/hub"
	"skyrim-mp/server/internal/recording"
	"skyrim-mp/server/internal/validate"
	"skyrim-mp/server/internal/wire/pb"
	"skyrim-mp/server/internal/world"
)

var ErrNotFound = errors.New("playback not found")

// HubPlayer is a virtual player in the hub. Its messages take the same hub
// input as a connection's, in order.
type HubPlayer struct {
	loop *hub.Loop
	slot uint32
	log  *slog.Logger
}

// Spawn adds a virtual player to the hub.
func Spawn(loop *hub.Loop, kind world.Kind, log *slog.Logger) (*HubPlayer, bool) {
	p := &HubPlayer{loop: loop, log: log}
	if !loop.Call(func(h *hub.Hub) { p.slot = h.Connect(kind, nil) }) {
		return nil, false
	}
	return p, true
}

func (p *HubPlayer) Slot() uint32 { return p.slot }

func (p *HubPlayer) Send(m *pb.ClientMessage) {
	p.loop.Submit(func(h *hub.Hub) {
		if err := h.Handle(p.slot, m); err != nil && !errors.Is(err, hub.ErrRateLimited) {
			p.log.Debug("virtual player message rejected", "slot", p.slot, "error", err)
		}
	})
}

func (p *HubPlayer) Leave() {
	p.loop.Submit(func(h *hub.Hub) { h.Disconnect(p.slot) })
}

// Manager starts, finds, and stops panel replays, each its own virtual player.
type Manager struct {
	store    *recording.Store
	loop     *hub.Loop
	log      *slog.Logger
	onChange func()

	mu       sync.Mutex
	sessions map[string]*Session
	order    []string
}

func NewManager(store *recording.Store, loop *hub.Loop, log *slog.Logger, onChange func()) *Manager {
	return &Manager{store: store, loop: loop, log: log, onChange: onChange, sessions: make(map[string]*Session)}
}

// Open replays a recording as a new virtual player.
func (m *Manager) Open(recordingID string, o Options) (*Session, error) {
	r, err := m.store.Load(recordingID)
	if err != nil {
		return nil, err
	}
	o.OnChange = m.onChange
	player, ok := Spawn(m.loop, world.KindReplay, m.log)
	if !ok {
		return nil, errors.New("server is stopping")
	}
	id := randomID()
	session, err := NewSession(id, strconv.FormatUint(uint64(player.Slot()), 10), r, player, o)
	if err != nil {
		player.Leave()
		return nil, err
	}
	m.mu.Lock()
	m.sessions[id] = session
	m.order = append(m.order, id)
	m.mu.Unlock()
	session.Start()
	m.log.Info("playback started", "id", id, "recording", recordingID, "slot", player.Slot())
	m.onChange()
	return session, nil
}

func (m *Manager) Get(id string) (*Session, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	if s, ok := m.sessions[id]; ok {
		return s, nil
	}
	return nil, ErrNotFound
}

func (m *Manager) Sessions() []*Session {
	m.mu.Lock()
	defer m.mu.Unlock()
	sessions := make([]*Session, 0, len(m.order))
	for _, id := range m.order {
		sessions = append(sessions, m.sessions[id])
	}
	return sessions
}

func (m *Manager) Close(id string) error {
	m.mu.Lock()
	s, ok := m.sessions[id]
	if ok {
		delete(m.sessions, id)
		m.order = slices.DeleteFunc(m.order, func(other string) bool { return other == id })
	}
	m.mu.Unlock()
	if !ok {
		return ErrNotFound
	}
	s.Close()
	m.log.Info("playback stopped", "id", id)
	m.onChange()
	return nil
}

func (m *Manager) CloseAll() {
	for _, s := range m.Sessions() {
		m.Close(s.ID)
	}
}

func randomID() string {
	var b [4]byte
	rand.Read(b[:])
	return hex.EncodeToString(b[:])
}

// BotName numbers a base name from 1 and shortens it to the protocol's name
// limit.
func BotName(base string, index int) string {
	suffix := " #" + strconv.Itoa(index+1)
	budget := validate.MaximumNameBytes - len(suffix)
	for len(base) > budget {
		// Drop whole runes so the result stays valid UTF-8.
		_, size := utf8.DecodeLastRuneInString(base)
		base = base[:len(base)-size]
	}
	return base + suffix
}
