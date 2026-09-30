// Package session serves player connections: one binary WebSocket message
// per protobuf envelope. Each connection decodes and validates on its own
// reader goroutine, submits accepted messages to the hub, and writes its
// outbox on a writer goroutine.
package session

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"time"

	"github.com/coder/websocket"
	"google.golang.org/protobuf/proto"

	"skyrim-mp/server/internal/config"
	"skyrim-mp/server/internal/delivery"
	"skyrim-mp/server/internal/hub"
	"skyrim-mp/server/internal/limit"
	"skyrim-mp/server/internal/validate"
	"skyrim-mp/server/internal/wire"
	"skyrim-mp/server/internal/wire/pb"
	"skyrim-mp/server/internal/world"
)

// The largest valid ClientMessage is about 12.9 KB.
const readLimitBytes = 16 * 1024

const (
	// StatusProtocolMismatch closes a connection whose Hello names another
	// protocol version. The reason is "protocol <server version>".
	StatusProtocolMismatch websocket.StatusCode = 4001
	// StatusBacklog closes a connection that stopped reading.
	StatusBacklog websocket.StatusCode = 4002
	// StatusRateLimited closes a connection that sends too fast.
	StatusRateLimited websocket.StatusCode = 4003
)

type Server struct {
	loop   *hub.Loop
	config config.Config
	log    *slog.Logger
}

func NewServer(loop *hub.Loop, c config.Config, log *slog.Logger) *Server {
	return &Server{loop: loop, config: c, log: log}
}

func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{
		CompressionMode: websocket.CompressionDisabled,
		// Game clients send no Origin; browsers are not players.
		InsecureSkipVerify: true,
	})
	if err != nil {
		return
	}
	conn.SetReadLimit(readLimitBytes)
	s.serve(r.Context(), conn, r.RemoteAddr)
}

func (s *Server) serve(parent context.Context, conn *websocket.Conn, address string) {
	ctx, cancel := context.WithCancel(parent)
	defer cancel()
	defer conn.CloseNow()

	if err := s.readHello(ctx, conn); err != nil {
		s.log.Info("player refused", "address", address, "error", err)
		return
	}

	outbox := delivery.NewOutbox(s.config.Limits.OrderedBacklog)
	var slot uint32
	if !s.loop.Call(func(h *hub.Hub) { slot = h.Connect(world.KindClient, outbox) }) {
		conn.Close(websocket.StatusGoingAway, "server stopping")
		return
	}
	log := s.log.With("slot", slot)
	log.Info("player connected", "address", address)

	writerDone := make(chan error, 1)
	go func() {
		err := s.write(ctx, conn, outbox)
		if errors.Is(err, errBacklog) {
			conn.Close(StatusBacklog, "backlog")
		}
		cancel()
		writerDone <- err
	}()
	go s.keepAlive(ctx, conn, cancel)

	err := s.read(ctx, conn, slot, outbox, log)
	if errors.Is(err, errRateLimited) {
		conn.Close(StatusRateLimited, "rate limited")
	}
	cancel()
	s.loop.Submit(func(h *hub.Hub) { h.Disconnect(slot) })
	outbox.Close()
	if writeErr := <-writerDone; errors.Is(writeErr, errBacklog) {
		err = writeErr
	}
	log.Info("player disconnected", "reason", err)
}

func (s *Server) readHello(ctx context.Context, conn *websocket.Conn) error {
	timeout := time.Duration(s.config.Limits.HelloTimeoutSeconds * float64(time.Second))
	helloContext, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	typ, data, err := conn.Read(helloContext)
	if err != nil {
		return err
	}
	var message pb.ClientMessage
	if typ != websocket.MessageBinary || proto.Unmarshal(data, &message) != nil || message.GetHello() == nil {
		conn.Close(websocket.StatusPolicyViolation, "expected hello")
		return errors.New("first message is not a hello")
	}
	if version := message.GetHello().GetProtocolVersion(); version != uint32(pb.Protocol_PROTOCOL_VERSION) {
		conn.Close(StatusProtocolMismatch, fmt.Sprintf("protocol %d", pb.Protocol_PROTOCOL_VERSION))
		return fmt.Errorf("protocol %d, server has %d", version, pb.Protocol_PROTOCOL_VERSION)
	}
	return nil
}

var (
	errBacklog     = errors.New("ordered backlog overflowed")
	errRateLimited = errors.New("message rate exceeded")
)

func (s *Server) read(ctx context.Context, conn *websocket.Conn, slot uint32, outbox *delivery.Outbox, log *slog.Logger) error {
	var rate limit.TokenBucket
	rejections := newRejectionLog(log)
	for {
		typ, data, err := conn.Read(ctx)
		if err != nil {
			return err
		}
		if !rate.Allow(time.Now(), s.config.Limits.MessagesPerSecond, s.config.Limits.MessageBurst) {
			return errRateLimited
		}
		message := &pb.ClientMessage{}
		if typ != websocket.MessageBinary || proto.Unmarshal(data, message) != nil {
			rejections.note("undecodable message")
			continue
		}
		if ping := message.GetPing(); ping != nil {
			outbox.AppendGlobal(wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_Result{
				Result: &pb.Result{RequestId: ping.GetRequestId(), Ok: true}}}))
			continue
		}
		if err := validate.Message(message); err != nil {
			rejections.note(err.Error())
			continue
		}
		s.loop.Submit(func(h *hub.Hub) {
			if err := h.Handle(slot, message); err != nil {
				rejections.note(err.Error())
			}
		})
	}
}

func (s *Server) write(ctx context.Context, conn *websocket.Conn, outbox *delivery.Outbox) error {
	var batch delivery.Batch
	timeout := time.Duration(s.config.Liveness.TimeoutSeconds * float64(time.Second))
	send := func(frame []byte) error {
		writeContext, cancel := context.WithTimeout(ctx, timeout)
		defer cancel()
		return conn.Write(writeContext, websocket.MessageBinary, frame)
	}
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-outbox.Ready():
		}
		if !outbox.Drain(&batch) {
			if ctx.Err() == nil {
				return errBacklog
			}
			return ctx.Err()
		}
		if err := batch.Write(send); err != nil {
			return err
		}
	}
}

// keepAlive pings on the transport, independent of the game loop: a paused
// game sends nothing for as long as it stays paused.
func (s *Server) keepAlive(ctx context.Context, conn *websocket.Conn, cancel context.CancelFunc) {
	interval := time.Duration(s.config.Liveness.PingIntervalSeconds * float64(time.Second))
	timeout := time.Duration(s.config.Liveness.TimeoutSeconds * float64(time.Second))
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
		pingContext, cancelPing := context.WithTimeout(ctx, timeout)
		err := conn.Ping(pingContext)
		cancelPing()
		if err != nil {
			cancel()
			return
		}
	}
}
