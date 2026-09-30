package session

import (
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
	server := httptest.NewServer(NewServer(loop, c, log))
	t.Cleanup(func() {
		server.CloseClientConnections()
		server.Close()
		cancel()
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

func hello(version uint32) *pb.ClientMessage {
	return &pb.ClientMessage{Message: &pb.ClientMessage_Hello{Hello: &pb.Hello{ProtocolVersion: version}}}
}

func join(t *testing.T, s *testServer) (*client, uint32) {
	t.Helper()
	c := dial(t, s)
	c.send(hello(uint32(pb.Protocol_PROTOCOL_VERSION)))
	welcome := c.receive().GetWelcome()
	if welcome == nil {
		t.Fatal("no welcome")
	}
	return c, welcome.Slot
}

func TestPlayersSeeEachOther(t *testing.T) {
	s := startServer(t, config.Default())
	a, slotA := join(t, s)
	b, _ := join(t, s)
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
	a, _ := join(t, s)
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
	c.send(hello(99))
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
	a, _ := join(t, s)
	for range 10 {
		a.send(&pb.ClientMessage{Message: &pb.ClientMessage_Enter{Enter: &pb.Enter{}}})
	}
	if code, _ := closeStatus(t, a); code != StatusRateLimited {
		t.Fatalf("got %d", code)
	}
}
