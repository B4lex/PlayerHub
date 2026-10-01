package session

import (
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"log/slog"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/coder/websocket"
	"google.golang.org/protobuf/proto"

	"skyrim-mp/server/internal/auth"
	"skyrim-mp/server/internal/config"
	"skyrim-mp/server/internal/hub"
	"skyrim-mp/server/internal/wire/pb"
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
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	conn, _, err := websocket.Dial(ctx, s.url, nil)
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

// tryReceive returns an error instead of failing the test, for a connection
// the server may refuse.
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

func hello(nickname string, token []byte) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Hello{Hello: &pb.Hello{
		ProtocolVersion: uint32(pb.Protocol_PROTOCOL_VERSION), Nickname: nickname, SessionToken: token}}}
}

// resume connects with a session's token, or without one for a new session.
func resume(t *testing.T, s *testServer, nickname string, token []byte) (*client, *pb.Welcome) {
	t.Helper()
	c := dial(t, s)
	c.send(hello(nickname, token))
	welcome := c.receive().GetWelcome()
	if welcome == nil {
		t.Fatal("no welcome")
	}
	// A new session is kept once its client is heard from.
	if welcome.GetSessionToken() != nil {
		c.send(&pb.ClientMessage{Message: &pb.ClientMessage_Ping{Ping: &pb.Ping{RequestId: 9999}}})
		if result := c.receive().GetResult(); result.GetRequestId() != 9999 {
			t.Fatalf("got %v", result)
		}
	}
	return c, welcome
}

func join(t *testing.T, s *testServer, nickname string) (*client, uint32) {
	t.Helper()
	c, welcome := resume(t, s, nickname, nil)
	return c, welcome.Slot
}

// refused connects and returns the status the server closed with.
func refused(t *testing.T, s *testServer, nickname string, token []byte) (websocket.StatusCode, string) {
	t.Helper()
	c := dial(t, s)
	c.send(hello(nickname, token))
	return closeStatus(t, c)
}

func place(c *client, x int32) {
	c.send(&pb.ClientMessage{Message: &pb.ClientMessage_Enter{Enter: &pb.Enter{}}})
	c.send(&pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: &pb.Context{WorldspaceId: 60, GraphEpoch: 1}}})
	c.send(&pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: &pb.Checkpoint{Sequence: 1, X: x}}})
}

func setNickname(id uint32, nickname string) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_SetNickname{SetNickname: &pb.SetNickname{RequestId: id, Nickname: nickname}}}
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
	c.send(&pb.ClientMessage{Message: &pb.ClientMessage_Hello{Hello: &pb.Hello{ProtocolVersion: 99, Nickname: "Lydia"}}})
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
	if issued.GetSessionId() == 0 || len(issued.GetSessionToken()) != 32 || issued.GetNickname() != "Lydia" {
		t.Fatalf("issued %v", issued)
	}
	first.conn.Close(websocket.StatusNormalClosure, "")

	_, resumed := resume(t, s, "Lydia", issued.GetSessionToken())
	if resumed.GetSessionId() != issued.GetSessionId() || resumed.GetSessionToken() != nil || resumed.GetSlot() == issued.GetSlot() {
		t.Fatalf("resumed %v after %v", resumed, issued)
	}

	// A token no session has, such as an expired one, gets a new session.
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
	if renamed.GetSessionId() != issued.GetSessionId() || renamed.GetNickname() != "Jordis" {
		t.Fatalf("renamed %v", renamed)
	}
	// The old name is free for another session.
	join(t, s, "Lydia")
}

func TestHelloIsRefused(t *testing.T) {
	s := startServer(t, config.Default())
	_, lydia := resume(t, s, "Lydia", nil)
	_, serana := resume(t, s, "Serana", nil)
	for name, c := range map[string]struct {
		nickname string
		token    []byte
		code     websocket.StatusCode
		reason   string
	}{
		"taken by a new session":     {"lydia", nil, StatusNicknameTaken, "nickname taken"},
		"taken by a resumed session": {"Lydia", serana.GetSessionToken(), StatusNicknameTaken, "nickname taken"},
		"no nickname":                {"", lydia.GetSessionToken(), StatusNicknameInvalid, "invalid nickname"},
		"surrounding space":          {" Aela", nil, StatusNicknameInvalid, "invalid nickname"},
		"control character":          {"Ae\x07la", nil, StatusNicknameInvalid, "invalid nickname"},
		"too long":                   {strings.Repeat("a", 129), nil, StatusNicknameInvalid, "invalid nickname"},
		"short token":                {"Aela", []byte{1, 2, 3}, websocket.StatusPolicyViolation, "invalid session token"},
	} {
		if code, reason := refused(t, s, c.nickname, c.token); code != c.code || reason != c.reason {
			t.Errorf("%s: got %d %q", name, code, reason)
		}
	}
	// A refused hello changed nothing.
	_, again := resume(t, s, "Serana", serana.GetSessionToken())
	if again.GetSessionId() != serana.GetSessionId() || again.GetNickname() != "Serana" {
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

func TestSetNickname(t *testing.T) {
	s := startServer(t, config.Default())
	a, issued := resume(t, s, "Lydia", nil)
	b, _ := join(t, s, "Serana")
	place(b, 0)
	a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Appearance{Appearance: &pb.Appearance{
		Revision: 1, Name: "Impostor", Race: &pb.FormRef{Plugin: "Skyrim.esm", FormId: 1}, Weight: 50, Height: 1,
		BodyTint: &pb.Color{}, FaceMorphs: make([]float32, 19), FaceParts: make([]int32, 4)}}})
	place(a, 10)
	if enter := b.receive().GetPeerEnter(); enter.GetAppearance().GetName() != "Lydia" {
		t.Fatalf("got %v", enter)
	}
	if enter := a.receive().GetPeerEnter(); enter == nil {
		t.Fatal("a did not receive b")
	}

	a.send(setNickname(1, "serana"))
	if result := a.receive().GetResult(); result.GetRequestId() != 1 || result.GetOk() || result.GetError() != "nickname taken" {
		t.Fatalf("got %v", result)
	}
	a.send(setNickname(2, " Jordis"))
	if result := a.receive().GetResult(); result.GetRequestId() != 2 || result.GetOk() || result.GetError() != "invalid nickname" {
		t.Fatalf("got %v", result)
	}
	a.send(setNickname(3, "Jordis"))
	if result := a.receive().GetResult(); result.GetRequestId() != 3 || !result.GetOk() {
		t.Fatalf("got %v", result)
	}
	renamed := b.receive().GetPeerAppearance()
	if renamed.GetSlot() != issued.GetSlot() || renamed.GetAppearance().GetName() != "Jordis" || renamed.GetAppearance().GetRevision() != 2 {
		t.Fatalf("got %v", renamed)
	}
	// Renames are limited apart from the message rate; refused ones count.
	a.send(setNickname(4, "Mjoll"))
	if result := a.receive().GetResult(); result.GetOk() || result.GetError() != "rate limited" {
		t.Fatalf("got %v", result)
	}

	// The rename is the session's, so it outlives the connection.
	a.conn.Close(websocket.StatusNormalClosure, "")
	if code, _ := refused(t, s, "Jordis", nil); code != StatusNicknameTaken {
		t.Fatalf("the renamed session lost its name: %d", code)
	}
	_, resumed := resume(t, s, "Jordis", issued.GetSessionToken())
	if resumed.GetSessionId() != issued.GetSessionId() || resumed.GetNickname() != "Jordis" {
		t.Fatalf("resumed %v", resumed)
	}
}

func TestNewSessionThatIsNeverUsedIsDeleted(t *testing.T) {
	s := startServer(t, config.Default())
	// The welcome may never reach a client whose connection drops at once. It
	// could not present the token, and would find its own nickname taken.
	c := dial(t, s)
	c.send(hello("Lydia", nil))
	lost := c.receive().GetWelcome()
	c.conn.Close(websocket.StatusNormalClosure, "")

	// The nickname is free once the server has noticed the close.
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
			t.Fatalf("the unused session kept its nickname: %v", err)
		}
		time.Sleep(10 * time.Millisecond)
	}
}

func TestUsedSessionIsKept(t *testing.T) {
	s := startServer(t, config.Default())
	c, issued := resume(t, s, "Lydia", nil)
	c.conn.Close(websocket.StatusNormalClosure, "")
	// Whenever the server notices the close, the session is still there.
	for range 20 {
		if code, _ := refused(t, s, "Lydia", nil); code != StatusNicknameTaken {
			t.Fatalf("the nickname of a used session was given away: %d", code)
		}
		time.Sleep(5 * time.Millisecond)
	}
	if _, resumed := resume(t, s, "Lydia", issued.GetSessionToken()); resumed.GetSessionId() != issued.GetSessionId() {
		t.Fatalf("resumed %v after %v", resumed, issued)
	}
}

func TestNewSessionResumedBeforeItWasUsedIsKept(t *testing.T) {
	s := startServer(t, config.Default())
	// The client received the welcome, lost the connection before sending
	// anything, and came back with the token before the server noticed.
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
	// The first connection has ended on the server once a rename, which the
	// store checks, has been answered after it.
	for id := uint32(1); id <= 2; id++ {
		time.Sleep(50 * time.Millisecond)
		second.send(setNickname(id, fmt.Sprintf("Lydia %d", id)))
		if result := second.receive().GetResult(); result.GetRequestId() != id || !result.GetOk() {
			t.Fatalf("the session of the second connection is gone: %v", result)
		}
	}
	second.conn.Close(websocket.StatusNormalClosure, "")
	if _, again := resume(t, s, "Lydia 2", issued.GetSessionToken()); again.GetSessionId() != issued.GetSessionId() {
		t.Fatalf("resumed %v after %v", again, issued)
	}
}
