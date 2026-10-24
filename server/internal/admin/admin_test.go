package admin

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
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
	identities, err := auth.Open("", 30*24*time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	connections := session.NewServer(loop, identities, config.Default(), log)
	c := &Context{Loop: loop, Store: store, Captures: captures, Playbacks: playbacks, Batches: batches,
		Sessions: identities, Connections: connections, StartedAt: time.Now(), OnChange: func() {}}
	mux := http.NewServeMux()
	mux.Handle("GET /ws", connections)
	c.Routes(mux)
	server := httptest.NewServer(Guard(mux, []string{"Panel.lan"}))
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
	if body != nil {
		request.Header.Set("Content-Type", "application/json")
	}
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
	p, _ := s.joinWith(displayName, nil)
	return p
}

func (s *stack) joinWith(displayName string, token []byte) (*player, *pb.Welcome) {
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
		ProtocolVersion: uint32(pb.Protocol_PROTOCOL_VERSION), DisplayName: displayName, SessionToken: token}}})
	welcome := p.receive().GetWelcome()
	if welcome == nil {
		s.t.Fatalf("%s got no welcome", displayName)
	}
	p.slot = welcome.GetSlot()
	return p, welcome
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
	b.send(&pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: &pb.Context{Worldspace: &pb.FormRef{Plugin: "Skyrim.esm", FormId: 60}, GraphEpoch: 1}}})
	b.send(&pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: &pb.Checkpoint{Sequence: 1}}})
	a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Appearance{Appearance: &pb.Appearance{
		Revision: 1, Race: &pb.FormRef{Plugin: "Skyrim.esm", FormId: 1}, Weight: 50, Height: 1,
		BodyTint: &pb.Color{}, FaceMorphs: make([]float32, 19), FaceParts: make([]int32, 4)}}})
	a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Enter{Enter: &pb.Enter{}}})
	a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: &pb.Context{Worldspace: &pb.FormRef{Plugin: "Skyrim.esm", FormId: 60}, GraphEpoch: 3}}})
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
	a.until(func(m *pb.ServerMessage) bool { return m.GetPong() != nil })
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
	if enter.Appearance.GetName() != "Lydia" || enter.State.GetX() != 5 || enter.Context.GetWorldspace().GetFormId() != 60 {
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

func TestBrowserRequestsFromOtherSites(t *testing.T) {
	s := startStack(t)
	for _, c := range []struct {
		name, method, path, body string
		headers                  map[string]string
		host                     string
		status                   int
	}{
		{"loopback address", "GET", "/api/state", "", nil, "", 200},
		{"localhost", "GET", "/api/state", "", nil, "localhost:3001", 200},
		{"IPv6 literal", "GET", "/api/state", "", nil, "[::1]:3001", 200},
		{"LAN address", "GET", "/api/state", "", nil, "192.168.1.20:3001", 200},
		{"configured host", "GET", "/api/state", "", nil, "panel.LAN:3001", 200},
		{"rebound domain", "GET", "/api/state", "", nil, "attacker.example:3001", 403},
		{"rebound domain without port", "POST", "/api/sessions", `{"displayName":"Lydia"}`,
			map[string]string{"Content-Type": "application/json"}, "attacker.example", 403},
		{"text body", "POST", "/api/batches", `{"recordingId":"missing","players":3}`,
			map[string]string{"Content-Type": "text/plain"}, "", 415},
		{"no content type", "POST", "/api/sessions", `{"displayName":"Lydia"}`, nil, "", 415},
		{"JSON with charset", "POST", "/api/batches", `{"recordingId":"missing","players":3}`,
			map[string]string{"Content-Type": "application/json; charset=utf-8"}, "", 404},
		{"cross-site fetch", "POST", "/api/batches", `{"recordingId":"missing","players":3}`,
			map[string]string{"Content-Type": "application/json", "Sec-Fetch-Site": "cross-site"}, "", 403},
		{"cross-origin without fetch metadata", "POST", "/api/batches", `{"recordingId":"missing","players":3}`,
			map[string]string{"Content-Type": "application/json", "Origin": "https://attacker.example"}, "", 403},
		{"cross-site GET", "GET", "/api/state", "", map[string]string{"Sec-Fetch-Site": "cross-site"}, "", 403},
		{"cross-site navigation", "GET", "/admin/", "",
			map[string]string{"Sec-Fetch-Site": "cross-site", "Sec-Fetch-Mode": "navigate"}, "", 403},
		{"other port on the same host", "GET", "/api/recordings/x", "", map[string]string{"Sec-Fetch-Site": "same-site"}, "", 403},
		{"typed URL", "GET", "/api/state", "", map[string]string{"Sec-Fetch-Site": "none"}, "", 200},
		{"same-origin fetch", "POST", "/api/batches", `{"recordingId":"missing","players":3}`,
			map[string]string{"Content-Type": "application/json", "Sec-Fetch-Site": "same-origin"}, "", 404},
	} {
		request, _ := http.NewRequest(c.method, s.server.URL+c.path, strings.NewReader(c.body))
		for key, value := range c.headers {
			request.Header.Set(key, value)
		}
		if c.host != "" {
			request.Host = c.host
		}
		response, err := http.DefaultClient.Do(request)
		if err != nil {
			t.Fatal(err)
		}
		response.Body.Close()
		if response.StatusCode != c.status {
			t.Errorf("%s: got %d, want %d", c.name, response.StatusCode, c.status)
		}
	}
	if status := s.call("POST", "/api/sessions", map[string]string{"displayName": "Lydia"}, nil); status != 201 {
		t.Errorf("a refused request took the display name: issuing it got %d", status)
	}
}

type issuedSession struct {
	SessionID   string `json:"sessionId"`
	DisplayName string `json:"displayName"`
	Token       []byte `json:"token"`
}

func TestIssuedSessionsSkipTheAllowance(t *testing.T) {
	s := startStack(t)
	allowance := int(config.Default().Limits.NewSessionsPerAddressPerMonth)
	var first issuedSession
	for i := range allowance + 2 {
		var issued issuedSession
		if status := s.call("POST", "/api/sessions", map[string]string{"displayName": fmt.Sprintf("Bot_%d", i)}, &issued); status != 201 {
			t.Fatalf("issue %d: %d", i, status)
		}
		if i == 0 {
			first = issued
		}
	}
	if len(first.Token) != 32 || first.DisplayName != "Bot_0" {
		t.Fatalf("issued %+v", first)
	}

	// Resuming answers the same session and token, which a hello then resumes too.
	var resumed issuedSession
	if status := s.call("POST", "/api/sessions", map[string]any{"displayName": "Bot_0", "token": first.Token}, &resumed); status != 200 {
		t.Fatalf("resume: %d", status)
	}
	if resumed.SessionID != first.SessionID || !bytes.Equal(resumed.Token, first.Token) {
		t.Fatalf("resumed %+v, issued %+v", resumed, first)
	}
	_, welcome := s.joinWith("Bot_0", first.Token)
	if fmt.Sprint(welcome.GetSessionId()) != first.SessionID || welcome.GetSessionToken() != nil {
		t.Fatalf("welcome %v for %+v", welcome, first)
	}

	for _, c := range []struct {
		body   map[string]any
		status int
	}{
		{map[string]any{"displayName": "bot_1"}, 409},
		{map[string]any{"displayName": "B"}, 422},
		{map[string]any{"displayName": "Bot_9", "token": []byte{1, 2, 3}}, 422},
		{map[string]any{"displayName": "Bot_9", "slot": 1}, 422},
	} {
		var body map[string]string
		if status := s.call("POST", "/api/sessions", c.body, &body); status != c.status {
			t.Errorf("%v: got %d, want %d (%v)", c.body, status, c.status, body)
		}
	}
}

func (p *player) place(x int32) {
	p.send(&pb.ClientMessage{Message: &pb.ClientMessage_Enter{Enter: &pb.Enter{}}})
	p.send(&pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: &pb.Context{Worldspace: &pb.FormRef{Plugin: "Skyrim.esm", FormId: 60}, GraphEpoch: 1}}})
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

func TestPlayersKeepJoinOrder(t *testing.T) {
	s := startStack(t)
	var want []string
	for _, name := range []string{"Lydia", "Serana", "Aela"} {
		want = append(want, slotID(s.join(name).slot))
	}
	for range 20 {
		var state State
		s.call("GET", "/api/state", nil, &state)
		var got []string
		for _, p := range state.Players {
			got = append(got, p.ID)
		}
		if fmt.Sprint(got) != fmt.Sprint(want) {
			t.Fatalf("players %v, want join order %v", got, want)
		}
	}
}

func (p *player) closed() websocket.StatusCode {
	p.t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	for {
		if _, _, err := p.conn.Read(ctx); err != nil {
			return websocket.CloseStatus(err)
		}
	}
}

func TestKickAndBan(t *testing.T) {
	s := startStack(t)
	kicked, welcome := s.joinWith("Lydia", nil)
	// A new session closed before its client sent anything is deleted.
	kicked.send(&pb.ClientMessage{Message: &pb.ClientMessage_Ping{Ping: &pb.Ping{RequestId: 1}}})
	kicked.until(func(m *pb.ServerMessage) bool { return m.GetPong() != nil })
	var state State
	s.call("GET", "/api/state", nil, &state)
	if len(state.Players) != 1 || state.Players[0].Address == nil || *state.Players[0].Address != "127.0.0.1" ||
		len(state.Bans) != 0 {
		t.Fatalf("state %+v", state)
	}
	if status := s.call("POST", "/api/players/999/kick", nil, nil); status != 404 {
		t.Fatalf("kick nobody: %d", status)
	}
	if status := s.call("POST", "/api/players/"+slotID(kicked.slot)+"/kick", nil, nil); status != 204 {
		t.Fatalf("kick: %d", status)
	}
	if code := kicked.closed(); code != session.StatusKicked {
		t.Fatalf("kicked with %d", code)
	}

	banned, _ := s.joinWith("Lydia", welcome.GetSessionToken())
	id := slotID(banned.slot)
	for _, c := range []struct {
		body   any
		status int
	}{
		{map[string]any{}, 422},
		{map[string]any{"address": "yes"}, 422},
		{map[string]any{"address": true}, 422},
	} {
		var body map[string]string
		if status := s.call("POST", "/api/players/"+id+"/ban", c.body, &body); status != c.status {
			t.Errorf("%v: got %d, want %d (%v)", c.body, status, c.status, body)
		}
	}
	if status := s.call("POST", "/api/players/999/ban", map[string]bool{"address": false}, nil); status != 404 {
		t.Fatalf("ban nobody: %d", status)
	}
	var ban Ban
	if status := s.call("POST", "/api/players/"+id+"/ban", map[string]bool{"address": false}, &ban); status != 201 {
		t.Fatalf("ban: %d", status)
	}
	if ban.DisplayName != "Lydia" || ban.SessionID != fmt.Sprint(welcome.GetSessionId()) || ban.Address != nil {
		t.Fatalf("ban %+v", ban)
	}
	if code := banned.closed(); code != session.StatusBanned {
		t.Fatalf("banned with %d", code)
	}
	token := map[string]any{"displayName": "Lydia", "token": welcome.GetSessionToken()}
	if status := s.call("POST", "/api/sessions", token, nil); status != 403 {
		t.Fatalf("issuing for a banned session: %d", status)
	}
	s.call("GET", "/api/state", nil, &state)
	if len(state.Bans) != 1 || state.Bans[0] != ban {
		t.Fatalf("bans %+v, want %+v", state.Bans, ban)
	}

	if status := s.call("DELETE", "/api/bans/"+ban.ID, nil, nil); status != 204 {
		t.Fatalf("lift: %d", status)
	}
	if status := s.call("DELETE", "/api/bans/"+ban.ID, nil, nil); status != 404 {
		t.Fatalf("lift again: %d", status)
	}
	s.joinWith("Lydia", welcome.GetSessionToken())
}
