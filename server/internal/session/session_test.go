package session

import (
	"bytes"
	"context"
	"errors"
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
	"playerhub/server/internal/wire/pb"
)

type testServer struct {
	url  string
	stop func()
}

func startServer(t *testing.T, c config.Config) *testServer {
	t.Helper()
	log := slog.New(slog.NewTextHandler(io.Discard, nil))
	loop := hub.NewLoop(hub.New(hub.Options{Logger: log}), c.SendRateHz)
	ctx, cancel := context.WithCancel(context.Background())
	go loop.Run(ctx)
	store, err := auth.Open("", 30*24*time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	server := httptest.NewServer(NewServer(loop, store, c, log))
	t.Cleanup(func() {
		server.CloseClientConnections()
		server.Close()
		cancel()
		store.Close()
	})
	return &testServer{url: "ws" + strings.TrimPrefix(server.URL, "http")}
}

type client struct {
	t    *testing.T
	conn *websocket.Conn
}

func dial(t *testing.T, s *testServer) *client {
	t.Helper()
	return dialWith(t, s, nil)
}

func dialWith(t *testing.T, s *testServer, header http.Header) *client {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	conn, _, err := websocket.Dial(ctx, s.url, &websocket.DialOptions{HTTPHeader: header})
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { conn.CloseNow() })
	return &client{t: t, conn: conn}
}

func (c *client) send(m *pb.ClientMessage) {
	c.t.Helper()
	data, err := proto.Marshal(m)
	if err != nil {
		c.t.Fatal(err)
	}
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if err := c.conn.Write(ctx, websocket.MessageBinary, data); err != nil {
		c.t.Fatal(err)
	}
}

func (c *client) receive() *pb.ServerMessage {
	c.t.Helper()
	for {
		if m := c.receiveAny(); m.GetRoster() == nil {
			return m
		}
	}
}

func (c *client) receiveAny() *pb.ServerMessage {
	c.t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_, data, err := c.conn.Read(ctx)
	if err != nil {
		c.t.Fatal(err)
	}
	var m pb.ServerMessage
	if err := proto.Unmarshal(data, &m); err != nil {
		c.t.Fatal(err)
	}
	return &m
}

func (c *client) tryReceive() (*pb.ServerMessage, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	_, data, err := c.conn.Read(ctx)
	if err != nil {
		return nil, err
	}
	var m pb.ServerMessage
	return &m, proto.Unmarshal(data, &m)
}

func hello(displayName string, token []byte) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Hello{Hello: &pb.Hello{
		ProtocolVersion: uint32(pb.Protocol_PROTOCOL_VERSION), DisplayName: displayName, SessionToken: token}}}
}

func resume(t *testing.T, s *testServer, displayName string, token []byte) (*client, *pb.Welcome) {
	t.Helper()
	c := dial(t, s)
	c.send(hello(displayName, token))
	welcome := c.receive().GetWelcome()
	if welcome == nil {
		t.Fatal("no welcome")
	}
	if welcome.GetSessionToken() != nil {
		c.send(&pb.ClientMessage{Message: &pb.ClientMessage_Ping{Ping: &pb.Ping{RequestId: 9999}}})
		if result := c.receive().GetResult(); result.GetRequestId() != 9999 {
			t.Fatalf("got %v", result)
		}
	}
	return c, welcome
}

func join(t *testing.T, s *testServer, displayName string) (*client, uint32) {
	t.Helper()
	c, welcome := resume(t, s, displayName, nil)
	return c, welcome.Slot
}

func refused(t *testing.T, s *testServer, displayName string, token []byte) (websocket.StatusCode, string) {
	t.Helper()
	c := dial(t, s)
	c.send(hello(displayName, token))
	return closeStatus(t, c)
}

func place(c *client, x int32) {
	c.send(&pb.ClientMessage{Message: &pb.ClientMessage_Enter{Enter: &pb.Enter{}}})
	c.send(&pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: &pb.Context{WorldspaceId: 60, GraphEpoch: 1}}})
	c.send(&pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: &pb.Checkpoint{Sequence: 1, X: x}}})
}

func TestPlayersSeeEachOther(t *testing.T) {
	s := startServer(t, config.Default())
	a, slotA := join(t, s, "Lydia")
	b, _ := join(t, s, "Serana")
	b.send(&pb.ClientMessage{Message: &pb.ClientMessage_Enter{Enter: &pb.Enter{}}})
	b.send(&pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: &pb.Context{WorldspaceId: 60, GraphEpoch: 1}}})
	b.send(&pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: &pb.Checkpoint{Sequence: 1}}})

	a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Enter{Enter: &pb.Enter{}}})
	a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: &pb.Context{WorldspaceId: 60, GraphEpoch: 1}}})
	a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: &pb.Checkpoint{Sequence: 1, X: 10}}})
	enter := b.receive().GetPeerEnter()
	if enter == nil || enter.Slot != slotA || enter.State.GetX() != 10 || enter.Context.GetWorldspaceId() != 60 {
		t.Fatalf("got %v", enter)
	}

	a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: &pb.Checkpoint{Sequence: 2, X: 20}}})
	states := b.receive().GetPeerStates()
	if len(states.GetStates()) != 1 || states.States[0].Player != slotA || states.States[0].X != 20 {
		t.Fatalf("got %v", states)
	}

	if enter := a.receive().GetPeerEnter(); enter == nil {
		t.Fatal("a did not receive b")
	}
	a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Ping{Ping: &pb.Ping{RequestId: 5}}})
	if result := a.receive().GetResult(); result.GetRequestId() != 5 || !result.GetOk() {
		t.Fatalf("got %v", result)
	}

	a.conn.Close(websocket.StatusNormalClosure, "")
	leave := b.receive().GetPeerLeave()
	if leave.GetSlot() != slotA || leave.GetReason() != pb.LeaveReason_LEAVE_REASON_DISCONNECTED {
		t.Fatalf("got %v", leave)
	}
}

func TestRosterFollowsConnections(t *testing.T) {
	s := startServer(t, config.Default())
	a := dial(t, s)
	a.send(hello("Lydia", nil))
	slotA := a.receiveAny().GetWelcome().GetSlot()
	if joined := a.receiveAny().GetRoster().GetJoined(); len(joined) != 1 || joined[0].GetSlot() != slotA || joined[0].GetName() != "Lydia" {
		t.Fatalf("got %v", joined)
	}
	b, slotB := join(t, s, "Serana")
	if joined := a.receiveAny().GetRoster().GetJoined(); len(joined) != 1 || joined[0].GetSlot() != slotB || joined[0].GetName() != "Serana" {
		t.Fatalf("got %v", joined)
	}
	b.conn.Close(websocket.StatusNormalClosure, "")
	if roster := a.receiveAny().GetRoster(); roster.GetLeft() != slotB || len(roster.GetJoined()) != 0 {
		t.Fatalf("got %v", roster)
	}
}

func TestInvalidMessagesAreDropped(t *testing.T) {
	s := startServer(t, config.Default())
	a, _ := join(t, s, "Lydia")
	a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: &pb.Checkpoint{Heading: 5000}}})
	a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Hello{Hello: &pb.Hello{}}})
	a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Ping{Ping: &pb.Ping{RequestId: 1}}})
	if a.receive().GetResult().GetRequestId() != 1 {
		t.Fatal("connection did not survive invalid messages")
	}
}

func closeStatus(t *testing.T, c *client) (websocket.StatusCode, string) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	for {
		_, _, err := c.conn.Read(ctx)
		if err != nil {
			var closeError websocket.CloseError
			if errors.As(err, &closeError) {
				return closeError.Code, closeError.Reason
			}
			t.Fatalf("closed without a status: %v", err)
		}
	}
}

func TestProtocolMismatchIsRefused(t *testing.T) {
	s := startServer(t, config.Default())
	c := dial(t, s)
	c.send(&pb.ClientMessage{Message: &pb.ClientMessage_Hello{Hello: &pb.Hello{ProtocolVersion: 99, DisplayName: "Lydia"}}})
	code, reason := closeStatus(t, c)
	if want := fmt.Sprintf("protocol %d", pb.Protocol_PROTOCOL_VERSION); code != StatusProtocolMismatch || reason != want {
		t.Fatalf("got %d %q", code, reason)
	}
}

func TestMissingHelloTimesOut(t *testing.T) {
	c := config.Default()
	c.Limits.HelloTimeoutSeconds = 0.1
	client := dial(t, startServer(t, c))
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	if _, _, err := client.conn.Read(ctx); err == nil {
		t.Fatal("connection stayed open without a hello")
	}
}

func TestFloodingIsRateLimited(t *testing.T) {
	c := config.Default()
	c.Limits.MessagesPerSecond, c.Limits.MessageBurst = 1, 5
	s := startServer(t, c)
	a, _ := join(t, s, "Lydia")
	for range 10 {
		a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Enter{Enter: &pb.Enter{}}})
	}
	if code, _ := closeStatus(t, a); code != StatusRateLimited {
		t.Fatalf("got %d", code)
	}
}

func TestHelloIssuesAndResumesASession(t *testing.T) {
	s := startServer(t, config.Default())
	first, issued := resume(t, s, "Lydia", nil)
	if issued.GetSessionId() == 0 || len(issued.GetSessionToken()) != 32 || issued.GetDisplayName() != "Lydia" {
		t.Fatalf("issued %v", issued)
	}
	first.conn.Close(websocket.StatusNormalClosure, "")

	_, resumed := resume(t, s, "Lydia", issued.GetSessionToken())
	if resumed.GetSessionId() != issued.GetSessionId() || resumed.GetSessionToken() != nil || resumed.GetSlot() == issued.GetSlot() {
		t.Fatalf("resumed %v after %v", resumed, issued)
	}

	_, other := resume(t, s, "Serana", bytes.Repeat([]byte{1}, 32))
	if other.GetSessionId() == issued.GetSessionId() || len(other.GetSessionToken()) != 32 {
		t.Fatalf("unknown token got %v", other)
	}
}

func TestHelloRenamesAResumedSession(t *testing.T) {
	s := startServer(t, config.Default())
	first, issued := resume(t, s, "Lydia", nil)
	first.conn.Close(websocket.StatusNormalClosure, "")
	_, renamed := resume(t, s, "Jordis", issued.GetSessionToken())
	if renamed.GetSessionId() != issued.GetSessionId() || renamed.GetDisplayName() != "Jordis" {
		t.Fatalf("renamed %v", renamed)
	}
	join(t, s, "Lydia")
}

func TestHelloIsRefused(t *testing.T) {
	s := startServer(t, config.Default())
	_, lydia := resume(t, s, "Lydia", nil)
	_, serana := resume(t, s, "Serana", nil)
	for name, c := range map[string]struct {
		displayName string
		token       []byte
		code        websocket.StatusCode
		reason      string
	}{
		"taken by a new session":     {"lydia", nil, StatusDisplayNameTaken, "display name taken"},
		"taken by a resumed session": {"Lydia", serana.GetSessionToken(), StatusDisplayNameTaken, "display name taken"},
		"no display name":            {"", lydia.GetSessionToken(), StatusDisplayNameInvalid, "invalid display name"},
		"a space":                    {"Aela Huntress", nil, StatusDisplayNameInvalid, "invalid display name"},
		"not Latin":                  {"Аела", nil, StatusDisplayNameInvalid, "invalid display name"},
		"control character":          {"Ae\x07la", nil, StatusDisplayNameInvalid, "invalid display name"},
		"too short":                  {"Ae", nil, StatusDisplayNameInvalid, "invalid display name"},
		"too long":                   {strings.Repeat("a", 16), nil, StatusDisplayNameInvalid, "invalid display name"},
		"short token":                {"Aela", []byte{1, 2, 3}, websocket.StatusPolicyViolation, "invalid session token"},
	} {
		if code, reason := refused(t, s, c.displayName, c.token); code != c.code || reason != c.reason {
			t.Errorf("%s: got %d %q", name, code, reason)
		}
	}
	_, again := resume(t, s, "Serana", serana.GetSessionToken())
	if again.GetSessionId() != serana.GetSessionId() || again.GetDisplayName() != "Serana" {
		t.Fatalf("after refusals: %v", again)
	}
}

func TestSessionConnectingAgainClosesItsEarlierConnection(t *testing.T) {
	s := startServer(t, config.Default())
	first, issued := resume(t, s, "Lydia", nil)
	watcher, _ := join(t, s, "Serana")
	place(watcher, 0)
	place(first, 10)
	if enter := watcher.receive().GetPeerEnter(); enter.GetSlot() != issued.GetSlot() {
		t.Fatalf("got %v", enter)
	}

	second, resumed := resume(t, s, "Lydia", issued.GetSessionToken())
	if code, reason := closeStatus(t, first); code != StatusReplaced || reason != "session connected elsewhere" {
		t.Fatalf("the earlier connection closed with %d %q", code, reason)
	}
	leave := watcher.receive().GetPeerLeave()
	if leave.GetSlot() != issued.GetSlot() || leave.GetReason() != pb.LeaveReason_LEAVE_REASON_DISCONNECTED {
		t.Fatalf("got %v", leave)
	}
	place(second, 20)
	if enter := watcher.receive().GetPeerEnter(); enter.GetSlot() != resumed.GetSlot() || enter.GetState().GetX() != 20 {
		t.Fatalf("got %v", enter)
	}
}

func TestNewSessionThatIsNeverUsedIsDeleted(t *testing.T) {
	s := startServer(t, config.Default())
	c := dial(t, s)
	c.send(hello("Lydia", nil))
	lost := c.receive().GetWelcome()
	c.conn.Close(websocket.StatusNormalClosure, "")

	deadline := time.Now().Add(5 * time.Second)
	for {
		probe := dial(t, s)
		probe.send(hello("Lydia", nil))
		message, err := probe.tryReceive()
		if welcome := message.GetWelcome(); err == nil && welcome != nil {
			if welcome.GetSessionId() == lost.GetSessionId() || welcome.GetSessionToken() == nil {
				t.Fatalf("got %v after %v", welcome, lost)
			}
			return
		}
		if time.Now().After(deadline) {
			t.Fatalf("the unused session kept its display name: %v", err)
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func TestUsedSessionIsKept(t *testing.T) {
	s := startServer(t, config.Default())
	c, issued := resume(t, s, "Lydia", nil)
	c.conn.Close(websocket.StatusNormalClosure, "")
	for range 20 {
		if code, _ := refused(t, s, "Lydia", nil); code != StatusDisplayNameTaken {
			t.Fatalf("the display name of a used session was given away: %d", code)
		}
		time.Sleep(5 * time.Millisecond)
	}
	if _, resumed := resume(t, s, "Lydia", issued.GetSessionToken()); resumed.GetSessionId() != issued.GetSessionId() {
		t.Fatalf("resumed %v after %v", resumed, issued)
	}
}

func TestNewSessionResumedBeforeItWasUsedIsKept(t *testing.T) {
	s := startServer(t, config.Default())
	first := dial(t, s)
	first.send(hello("Lydia", nil))
	issued := first.receive().GetWelcome()

	second, resumed := resume(t, s, "Lydia", issued.GetSessionToken())
	if resumed.GetSessionId() != issued.GetSessionId() {
		t.Fatalf("resumed %v after %v", resumed, issued)
	}
	if code, _ := closeStatus(t, first); code != StatusReplaced {
		t.Fatalf("the first connection closed with %d", code)
	}
	for range 20 {
		if code, _ := refused(t, s, "Lydia", nil); code != StatusDisplayNameTaken {
			t.Fatalf("the session of the second connection is gone: %d", code)
		}
		time.Sleep(5 * time.Millisecond)
	}
	second.conn.Close(websocket.StatusNormalClosure, "")
	if _, again := resume(t, s, "Lydia", issued.GetSessionToken()); again.GetSessionId() != issued.GetSessionId() {
		t.Fatalf("resumed %v after %v", again, issued)
	}
}

func TestHelloVanishesPlayers(t *testing.T) {
	s := startServer(t, config.Default())
	a, lydia := resume(t, s, "Lydia", nil)
	b := dial(t, s)
	vanishing := hello("Serana", nil)
	vanishing.GetHello().Vanished = []uint64{lydia.GetSessionId()}
	b.send(vanishing)
	if b.receive().GetWelcome() == nil {
		t.Fatal("no welcome")
	}
	place(b, 0)
	place(a, 10)
	if enter := a.receive().GetPeerEnter(); enter == nil {
		t.Fatal("the vanished player did not receive the other")
	}
	b.send(&pb.ClientMessage{Message: &pb.ClientMessage_Ping{Ping: &pb.Ping{RequestId: 5}}})
	if result := b.receive().GetResult(); result.GetRequestId() != 5 {
		t.Fatalf("received the vanished player: %v", result)
	}

	c := dial(t, s)
	refusing := hello("Aela", nil)
	refusing.GetHello().Vanished = []uint64{0}
	c.send(refusing)
	if code, reason := closeStatus(t, c); code != websocket.StatusPolicyViolation || reason != "invalid vanish" {
		t.Fatalf("got %d %q", code, reason)
	}
}

func TestBrowsersCannotJoin(t *testing.T) {
	s := startServer(t, config.Default())
	for _, tc := range []struct {
		origin string
		joins  bool
	}{
		{"", true},
		{"ws://127.0.0.1:3000", true},
		{"wss://play.example.com:443", true},
		{"https://evil.example.com", false},
		{"http://127.0.0.1:3000", false},
		{"null", false},
		{"chrome-extension://abcdef", false},
	} {
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		options := &websocket.DialOptions{HTTPHeader: http.Header{}}
		if tc.origin != "" {
			options.HTTPHeader.Set("Origin", tc.origin)
		}
		conn, response, err := websocket.Dial(ctx, s.url, options)
		cancel()
		if tc.joins {
			if err != nil {
				t.Fatalf("origin %q: %v", tc.origin, err)
			}
			conn.CloseNow()
			continue
		}
		if err == nil {
			conn.CloseNow()
			t.Fatalf("origin %q joined", tc.origin)
		}
		if response == nil || response.StatusCode != http.StatusForbidden {
			t.Fatalf("origin %q: got %v, want 403", tc.origin, err)
		}
	}
}

func TestFullServerRefusesNewPlayers(t *testing.T) {
	c := config.Default()
	c.Limits.MaxPlayers = 1
	s := startServer(t, c)
	first, lydia := resume(t, s, "Lydia", nil)
	if code, reason := refused(t, s, "Serana", nil); code != StatusServerFull || reason != "server full" {
		t.Fatalf("got %d %q", code, reason)
	}
	second, _ := resume(t, s, "Lydia", lydia.GetSessionToken())
	if code, _ := closeStatus(t, first); code != StatusReplaced {
		t.Fatalf("got %d", code)
	}
	second.conn.Close(websocket.StatusNormalClosure, "")

	deadline := time.Now().Add(5 * time.Second)
	for {
		c := dial(t, s)
		c.send(hello("Serana", nil))
		m, err := c.tryReceive()
		if err == nil && m.GetWelcome() != nil {
			break
		}
		if websocket.CloseStatus(err) != StatusServerFull || time.Now().After(deadline) {
			t.Fatalf("got %v, %v", m, err)
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func TestNewSessionsPerAddressAreLimited(t *testing.T) {
	c := config.Default()
	c.Limits.NewSessionsPerAddressPerMonth = 2
	s := startServer(t, c)
	first, lydia := resume(t, s, "Lydia", nil)
	join(t, s, "Serana")
	if code, reason := refused(t, s, "Aela", nil); code != StatusTooManyNewSessions || reason != "too many new sessions" {
		t.Fatalf("got %d %q", code, reason)
	}
	if code, _ := refused(t, s, "lydia", nil); code != StatusDisplayNameTaken {
		t.Fatalf("got %d", code)
	}
	first.conn.Close(websocket.StatusNormalClosure, "")
	resume(t, s, "Lydia", lydia.GetSessionToken())
}

func TestNewSessionsAreLimitedByForwardedAddress(t *testing.T) {
	c := config.Default()
	c.Limits.NewSessionsPerAddressPerMonth = 1
	c.TrustedProxies = []string{"127.0.0.0/8", "::1"}
	s := startServer(t, c)
	joinFrom := func(address, displayName string) *client {
		c := dialWith(t, s, http.Header{"X-Forwarded-For": {address}})
		c.send(hello(displayName, nil))
		return c
	}
	if joinFrom("203.0.113.1", "Lydia").receive().GetWelcome() == nil {
		t.Fatal("no welcome")
	}
	if joinFrom("203.0.113.2", "Serana").receive().GetWelcome() == nil {
		t.Fatal("another address shares the limit")
	}
	if code, _ := closeStatus(t, joinFrom("1.2.3.4, 203.0.113.1", "Aela")); code != StatusTooManyNewSessions {
		t.Fatalf("a forged hop escaped the limit: %d", code)
	}
}

func TestPendingHellosAreCapped(t *testing.T) {
	c := config.Default()
	c.Limits.MaxPendingHellos = 1
	s := startServer(t, c)
	waiting := dial(t, s)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	conn, response, err := websocket.Dial(ctx, s.url, nil)
	if err == nil {
		conn.CloseNow()
		t.Fatal("connected beyond the cap")
	}
	if response == nil || response.StatusCode != http.StatusServiceUnavailable {
		t.Fatalf("got %v, want 503", err)
	}
	waiting.send(hello("Lydia", nil))
	if waiting.receive().GetWelcome() == nil {
		t.Fatal("no welcome")
	}
	join(t, s, "Serana")
}
