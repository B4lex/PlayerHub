package admin

import (
	"bytes"
	"context"
	"encoding/json"
	"io"
	"log/slog"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
	"google.golang.org/protobuf/proto"

	"playerhub/server/internal/auth"
	"playerhub/server/internal/config"
	"playerhub/server/internal/hub"
	"playerhub/server/internal/loadtest"
	"playerhub/server/internal/playback"
	"playerhub/server/internal/recording"
	"playerhub/server/internal/session"
	"playerhub/server/internal/wire/pb"
)

type stack struct {
	t      *testing.T
	server *httptest.Server
}

func startStack(t *testing.T) *stack {
	t.Helper()
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	store := recording.NewStore(t.TempDir(), log)
	captures := recording.NewCaptures(store, log, func() {})
	loop := hub.NewLoop(hub.New(hub.Options{Recorder: captures, Logger: log}), 40)
	ctx, cancel := context.WithCancel(context.Background())
	go loop.Run(ctx)
	playbacks := playback.NewManager(store, loop, log, func() {})
	batches := loadtest.NewManager(store, loop, log, func() {})
	c := &Context{Loop: loop, Store: store, Captures: captures, Playbacks: playbacks, Batches: batches,
		StartedAt: time.Now(), OnChange: func() {}}
	mux := http.NewServeMux()
	identities, err := auth.Open("", 30*24*time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	mux.Handle("GET /ws", session.NewServer(loop, identities, config.Default(), log))
	c.Routes(mux)
	server := httptest.NewServer(mux)
	t.Cleanup(func() {
		playbacks.CloseAll()
		batches.CloseAll()
		captures.StopAll("shutdown")
		server.CloseClientConnections()
		server.Close()
		cancel()
		identities.Close()
	})
	return &stack{t: t, server: server}
}

func (s *stack) call(method, path string, body any, into any) int {
	s.t.Helper()
	var reader io.Reader
	if body != nil {
		data, _ := json.Marshal(body)
		reader = bytes.NewReader(data)
	}
	request, _ := http.NewRequest(method, s.server.URL+path, reader)
	response, err := http.DefaultClient.Do(request)
	if err != nil {
		s.t.Fatal(err)
	}
	defer response.Body.Close()
	if into != nil {
		json.NewDecoder(response.Body).Decode(into)
	}
	return response.StatusCode
}

type player struct {
	t    *testing.T
	conn *websocket.Conn
	slot uint32
}

func (s *stack) join(displayName string) *player {
	s.t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	conn, _, err := websocket.Dial(ctx, "ws"+strings.TrimPrefix(s.server.URL, "http")+"/ws", nil)
	if err != nil {
		s.t.Fatal(err)
	}
	s.t.Cleanup(func() { conn.CloseNow() })
	p := &player{t: s.t, conn: conn}
	p.send(&pb.ClientMessage{Message: &pb.ClientMessage_Hello{Hello: &pb.Hello{
		ProtocolVersion: uint32(pb.Protocol_PROTOCOL_VERSION), DisplayName: displayName}}})
	p.slot = p.receive().GetWelcome().GetSlot()
	return p
}

func (p *player) send(m *pb.ClientMessage) {
	p.t.Helper()
	data, _ := proto.Marshal(m)
	if err := p.conn.Write(context.Background(), websocket.MessageBinary, data); err != nil {
		p.t.Fatal(err)
	}
}

func (p *player) receive() *pb.ServerMessage {
	p.t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_, data, err := p.conn.Read(ctx)
	if err != nil {
		p.t.Fatal(err)
	}
	var m pb.ServerMessage
	proto.Unmarshal(data, &m)
	return &m
}

func (p *player) until(match func(*pb.ServerMessage) bool) *pb.ServerMessage {
	p.t.Helper()
	for {
		if m := p.receive(); match(m) {
			return m
		}
	}
}

func TestCaptureAndReplay(t *testing.T) {
	s := startStack(t)
	a := s.join("Lydia")
	b := s.join("Serana")
	b.send(&pb.ClientMessage{Message: &pb.ClientMessage_Enter{Enter: &pb.Enter{}}})
	b.send(&pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: &pb.Context{WorldspaceId: 60, GraphEpoch: 1}}})
	b.send(&pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: &pb.Checkpoint{Sequence: 1}}})
	a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Appearance{Appearance: &pb.Appearance{
		Revision: 1, Race: &pb.FormRef{Plugin: "Skyrim.esm", FormId: 1}, Weight: 50, Height: 1,
		BodyTint: &pb.Color{}, FaceMorphs: make([]float32, 19), FaceParts: make([]int32, 4)}}})
	a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Enter{Enter: &pb.Enter{}}})
	a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: &pb.Context{WorldspaceId: 60, GraphEpoch: 3}}})
	a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: &pb.Checkpoint{Sequence: 1, X: 5}}})
	b.until(func(m *pb.ServerMessage) bool { return m.GetPeerEnter() != nil })

	var state State
	s.call("GET", "/api/state", nil, &state)
	if len(state.Players) != 2 || state.Players[0].Name == "" || state.Players[1].Name == "" {
		t.Fatalf("players: %+v", state.Players)
	}

	var info recording.Info
	if status := s.call("POST", "/api/recordings", map[string]string{"playerId": "1", "label": "walk"}, &info); status != 201 {
		t.Fatalf("start capture: %d", status)
	}
	// Later checkpoints must not share the capture's first millisecond.
	time.Sleep(20 * time.Millisecond)
	for sequence := uint64(2); sequence <= 4; sequence++ {
		a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: &pb.Checkpoint{Sequence: sequence, X: int32(sequence) * 10, Speed: 100}}})
		time.Sleep(20 * time.Millisecond)
	}
	// Let the hub record the last checkpoint before stopping.
	a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Ping{Ping: &pb.Ping{RequestId: 1}}})
	a.until(func(m *pb.ServerMessage) bool { return m.GetResult() != nil })
	time.Sleep(50 * time.Millisecond)
	if status := s.call("POST", "/api/recordings/"+info.ID+"/stop", nil, &info); status != 200 {
		t.Fatalf("stop capture: %d", status)
	}
	if info.Status != recording.StatusComplete || info.MessageCount != 7 || info.Label != "walk" {
		t.Fatalf("recording: %+v", info)
	}

	var replay playback.State
	if status := s.call("POST", "/api/playbacks", map[string]any{"recordingId": info.ID, "autoplay": false}, &replay); status != 201 {
		t.Fatalf("open playback: %d", status)
	}
	enter := b.until(func(m *pb.ServerMessage) bool {
		return m.GetPeerEnter() != nil && m.GetPeerEnter().Slot != a.slot
	}).GetPeerEnter()
	if enter.Appearance.GetName() != "Lydia" || enter.State.GetX() != 5 || enter.Context.GetWorldspaceId() != 60 {
		t.Fatalf("replayed peer: %v", enter)
	}
	if status := s.call("DELETE", "/api/playbacks/"+replay.ID, nil, nil); status != 204 {
		t.Fatalf("close playback: %d", status)
	}
	leave := b.until(func(m *pb.ServerMessage) bool { return m.GetPeerLeave() != nil }).GetPeerLeave()
	if leave.Slot != enter.Slot || leave.Reason != pb.LeaveReason_LEAVE_REASON_DISCONNECTED {
		t.Fatalf("leave: %v", leave)
	}

	var detail struct {
		Markers []recording.Marker `json:"markers"`
	}
	if status := s.call("GET", "/api/recordings/"+info.ID, nil, &detail); status != 200 {
		t.Fatalf("recording detail: %d", status)
	}
	if status := s.call("DELETE", "/api/recordings/"+info.ID, nil, nil); status != 204 {
		t.Fatalf("delete: %d", status)
	}
}

func TestRequestValidation(t *testing.T) {
	s := startStack(t)
	for _, c := range []struct {
		method, path string
		body         any
		status       int
	}{
		{"POST", "/api/recordings", map[string]string{"playerId": "9"}, 404},
		{"POST", "/api/recordings", map[string]any{"playerId": "1", "unknown": true}, 422},
		{"POST", "/api/playbacks", map[string]any{"recordingId": "missing"}, 404},
		{"POST", "/api/playbacks", map[string]any{"recordingId": "missing", "rate": 9}, 422},
		{"POST", "/api/playbacks/nope/play", nil, 404},
		{"POST", "/api/batches", map[string]any{"recordingId": "missing", "players": 0}, 422},
		{"POST", "/api/batches", map[string]any{"recordingId": "missing", "players": 3}, 404},
		{"DELETE", "/api/recordings/../x", nil, 404},
	} {
		var body map[string]string
		if status := s.call(c.method, c.path, c.body, &body); status != c.status {
			t.Errorf("%s %s: got %d, want %d (%v)", c.method, c.path, status, c.status, body)
		}
	}
}

func (p *player) place(x int32) {
	p.send(&pb.ClientMessage{Message: &pb.ClientMessage_Enter{Enter: &pb.Enter{}}})
	p.send(&pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: &pb.Context{WorldspaceId: 60, GraphEpoch: 1}}})
	p.send(&pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: &pb.Checkpoint{Sequence: 1, X: x}}})
}

func TestHideAndExclude(t *testing.T) {
	s := startStack(t)
	a, b := s.join("Lydia"), s.join("Serana")
	a.place(0)
	b.place(10)
	b.until(func(m *pb.ServerMessage) bool { return m.GetPeerEnter() != nil })
	a.until(func(m *pb.ServerMessage) bool { return m.GetPeerEnter() != nil })

	id := func(p *player) string { return slotID(p.slot) }
	if status := s.call("PUT", "/api/players/"+id(a)+"/hidden", map[string]bool{"hidden": true}, nil); status != 200 {
		t.Fatalf("hide: %d", status)
	}
	if leave := b.until(func(m *pb.ServerMessage) bool { return m.GetPeerLeave() != nil }).GetPeerLeave(); leave.Reason != pb.LeaveReason_LEAVE_REASON_EXCLUDED {
		t.Fatalf("leave: %v", leave)
	}
	var state State
	s.call("GET", "/api/state", nil, &state)
	hidden := 0
	for _, p := range state.Players {
		if p.Hidden {
			hidden++
		}
	}
	if hidden != 1 {
		t.Fatalf("hidden players: %+v", state.Players)
	}
	s.call("PUT", "/api/players/"+id(a)+"/hidden", map[string]bool{"hidden": false}, nil)
	b.until(func(m *pb.ServerMessage) bool { return m.GetPeerEnter() != nil })

	exclusion := Exclusion{ViewerID: id(a), TargetID: id(b)}
	if status := s.call("POST", "/api/exclusions", exclusion, nil); status != 200 {
		t.Fatalf("exclude: %d", status)
	}
	a.until(func(m *pb.ServerMessage) bool { return m.GetPeerLeave() != nil })
	s.call("GET", "/api/state", nil, &state)
	if len(state.Exclusions) != 1 || state.Exclusions[0] != exclusion {
		t.Fatalf("exclusions: %+v", state.Exclusions)
	}
	if status := s.call("DELETE", "/api/exclusions", exclusion, nil); status != 200 {
		t.Fatalf("lift: %d", status)
	}
	a.until(func(m *pb.ServerMessage) bool { return m.GetPeerEnter() != nil })
	if status := s.call("POST", "/api/exclusions", Exclusion{ViewerID: id(a), TargetID: id(a)}, nil); status != 404 {
		t.Fatalf("self exclusion: %d", status)
	}
	if status := s.call("PUT", "/api/players/999/hidden", map[string]bool{"hidden": true}, nil); status != 404 {
		t.Fatalf("unknown player: %d", status)
	}
}
