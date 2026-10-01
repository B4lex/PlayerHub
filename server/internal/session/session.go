// Package session serves player connections: one binary WebSocket message
// per protobuf envelope. Each connection authenticates its player's session
// with the hello, decodes and validates on its own reader goroutine, submits
// accepted messages to the hub, and writes its outbox on a writer goroutine.
package session

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"sync"
	"time"

	"github.com/coder/websocket"
	"google.golang.org/protobuf/proto"

	"skyrim-mp/server/internal/auth"
	"skyrim-mp/server/internal/config"
	"skyrim-mp/server/internal/delivery"
	"skyrim-mp/server/internal/hub"
	"skyrim-mp/server/internal/limit"
	"skyrim-mp/server/internal/validate"
	"skyrim-mp/server/internal/wire"
	"skyrim-mp/server/internal/wire/pb"
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
	// StatusNicknameTaken closes a connection whose Hello asks for a nickname
	// another session holds.
	StatusNicknameTaken websocket.StatusCode = 4004
	// StatusNicknameInvalid closes a connection whose Hello has no nickname
	// or one that cannot be a nickname.
	StatusNicknameInvalid websocket.StatusCode = 4005
	// StatusReplaced closes a connection whose session connected again.
	StatusReplaced websocket.StatusCode = 4006
)

const (
	// A rename writes to the session store, so it is limited apart from the
	// message rate.
	renamesPerSecond = 0.2
	renameBurst      = 3
	// How often connected sessions are marked used and expired ones deleted.
	maintenanceInterval = time.Hour
)

type Server struct {
	loop   *hub.Loop
	store  *auth.Store
	config config.Config
	log    *slog.Logger
	// Nil without an egress budget.
	budget *delivery.Budget

	mu sync.Mutex
	// By slot. Set on the hub goroutine as a client connects, so a session
	// that connects again finds the connection it replaces.
	connections map[uint32]*connection
}

type connection struct {
	conn    *websocket.Conn
	cancel  context.CancelFunc
	session uint64
	// The client sent something after its hello, so it received the welcome.
	// Reader goroutine only.
	heard bool
}

func NewServer(loop *hub.Loop, store *auth.Store, c config.Config, log *slog.Logger) *Server {
	return &Server{loop: loop, store: store, config: c, log: log, connections: make(map[uint32]*connection)}
}

// UseBudget makes every connection's writes share b. Call before serving.
func (s *Server) UseBudget(b *delivery.Budget) {
	s.budget = b
}

// SetLoop sets the hub connections join. Call before serving.
func (s *Server) SetLoop(loop *hub.Loop) {
	s.loop = loop
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

	hello, err := s.readHello(ctx, conn)
	if err != nil {
		s.log.Info("player refused", "address", address, "error", err)
		return
	}

	identity, err := s.authenticate(conn, hello)
	if err != nil {
		s.log.Info("player refused", "address", address, "nickname", hello.GetNickname(), "error", err)
		return
	}

	outbox := delivery.NewOutbox(s.config.Limits.OrderedBacklog)
	if s.budget != nil {
		outbox.UseBudget(s.budget)
	}
	current := &connection{conn: conn, cancel: cancel, session: identity.Session}
	var slot uint32
	var replaced *connection
	if !s.loop.Call(func(h *hub.Hub) {
		var replacedSlot uint32
		slot, replacedSlot = h.ConnectClient(outbox, identity)
		h.SetViewRadius(slot, float64(hello.GetViewRadius()))
		s.mu.Lock()
		replaced = s.connections[replacedSlot]
		s.connections[slot] = current
		s.mu.Unlock()
	}) {
		conn.Close(websocket.StatusGoingAway, "server stopping")
		return
	}
	log := s.log.With("slot", slot, "session", identity.Session)
	log.Info("player connected", "address", address, "nickname", identity.Nickname, "new_session", identity.Token != nil)
	if replaced != nil {
		log.Info("session connected again; closing its earlier connection")
		// Closing waits for the peer, which may be gone.
		go replaced.replace()
	}

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

	err = s.read(ctx, current, slot, outbox, log)
	if errors.Is(err, errRateLimited) {
		conn.Close(StatusRateLimited, "rate limited")
	}
	cancel()
	s.loop.Submit(func(h *hub.Hub) { h.Disconnect(slot) })
	s.mu.Lock()
	delete(s.connections, slot)
	s.mu.Unlock()
	// A stopping server closes the store; Maintain marked the session used
	// within the last interval.
	if parent.Err() == nil {
		s.settle(current, identity.Token != nil, log)
	}
	outbox.Close()
	if writeErr := <-writerDone; errors.Is(writeErr, errBacklog) {
		err = writeErr
	}
	log.Info("player disconnected", "reason", err)
}

func (s *Server) readHello(ctx context.Context, conn *websocket.Conn) (*pb.Hello, error) {
	timeout := time.Duration(s.config.Limits.HelloTimeoutSeconds * float64(time.Second))
	helloContext, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	typ, data, err := conn.Read(helloContext)
	if err != nil {
		return nil, err
	}
	var message pb.ClientMessage
	if typ != websocket.MessageBinary || proto.Unmarshal(data, &message) != nil || message.GetHello() == nil {
		conn.Close(websocket.StatusPolicyViolation, "expected hello")
		return nil, errors.New("first message is not a hello")
	}
	hello := message.GetHello()
	if version := hello.GetProtocolVersion(); version != uint32(pb.Protocol_PROTOCOL_VERSION) {
		conn.Close(StatusProtocolMismatch, fmt.Sprintf("protocol %d", pb.Protocol_PROTOCOL_VERSION))
		return nil, fmt.Errorf("protocol %d, server has %d", version, pb.Protocol_PROTOCOL_VERSION)
	}
	return hello, nil
}

// authenticate resumes or issues the session the hello asks for. A refused
// connection is closed with the status that tells the client why.
func (s *Server) authenticate(conn *websocket.Conn, hello *pb.Hello) (hub.Identity, error) {
	if err := validate.Hello(hello); err != nil {
		if errors.Is(err, validate.ErrNickname) {
			conn.Close(StatusNicknameInvalid, err.Error())
		} else {
			conn.Close(websocket.StatusPolicyViolation, err.Error())
		}
		return hub.Identity{}, err
	}
	session, issued, err := s.store.Authenticate(hello.GetSessionToken(), hello.GetNickname(), time.Now())
	switch {
	case errors.Is(err, auth.ErrNicknameTaken):
		conn.Close(StatusNicknameTaken, err.Error())
		return hub.Identity{}, err
	case err != nil:
		s.log.Error("session store failed", "error", err)
		conn.Close(websocket.StatusInternalError, "session store unavailable")
		return hub.Identity{}, err
	}
	return hub.Identity{Session: session.ID, Nickname: session.Nickname, Token: issued}, nil
}

// settle ends a connection's use of its session. A client that never
// received the welcome of a new session cannot present its token, and the
// session would keep the nickname from its own player until it expired, so it
// is deleted, unless the token came back on another connection meanwhile.
func (s *Server) settle(c *connection, issued bool, log *slog.Logger) {
	if issued && !c.heard {
		abandoned, err := s.store.Abandon(c.session)
		if err != nil {
			log.Error("cannot delete the unused session", "error", err)
		}
		if abandoned {
			log.Info("new session was never used; deleted")
			return
		}
	}
	if err := s.store.Touch([]uint64{c.session}, time.Now()); err != nil {
		log.Error("cannot mark the session used", "error", err)
	}
}

func (c *connection) replace() {
	c.conn.Close(StatusReplaced, "session connected elsewhere")
	c.cancel()
}

// Maintain keeps the sessions of connected players alive and deletes expired
// ones, until ctx ends.
func (s *Server) Maintain(ctx context.Context) {
	ticker := time.NewTicker(maintenanceInterval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
		s.mu.Lock()
		sessions := make([]uint64, 0, len(s.connections))
		for _, c := range s.connections {
			sessions = append(sessions, c.session)
		}
		s.mu.Unlock()
		now := time.Now()
		if err := s.store.Touch(sessions, now); err != nil {
			s.log.Error("cannot mark connected sessions used", "error", err)
			continue
		}
		if purged, err := s.store.Purge(now); err != nil {
			s.log.Error("cannot delete expired sessions", "error", err)
		} else if purged > 0 {
			s.log.Info("expired sessions deleted", "count", purged)
		}
	}
}

var (
	errBacklog     = errors.New("ordered backlog overflowed")
	errRateLimited = errors.New("message rate exceeded")
	errUnavailable = errors.New("session store unavailable")
)

func (s *Server) read(ctx context.Context, c *connection, slot uint32, outbox *delivery.Outbox, log *slog.Logger) error {
	var rate, renames limit.TokenBucket
	rejections := newRejectionLog(log)
	for {
		typ, data, err := c.conn.Read(ctx)
		if err != nil {
			return err
		}
		c.heard = true
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
		if rename := message.GetSetNickname(); rename != nil {
			if err := s.rename(slot, c.session, rename, &renames, outbox); err != nil {
				rejections.note(err.Error())
			}
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

// rename gives the session another nickname and answers the request. The
// store decides; the hub then tells the peers.
func (s *Server) rename(slot uint32, session uint64, request *pb.SetNickname, renames *limit.TokenBucket, outbox *delivery.Outbox) error {
	nickname := request.GetNickname()
	now := time.Now()
	var err error
	switch {
	case !renames.Allow(now, renamesPerSecond, renameBurst):
		err = hub.ErrRateLimited
	case validate.Nickname(nickname) != nil:
		err = validate.ErrNickname
	default:
		err = s.store.Rename(session, nickname, now)
	}
	result := &pb.Result{RequestId: request.GetRequestId(), Ok: err == nil}
	if err != nil {
		if !errors.Is(err, hub.ErrRateLimited) && !errors.Is(err, validate.ErrNickname) && !errors.Is(err, auth.ErrNicknameTaken) {
			s.log.Error("cannot rename a session", "session", session, "error", err)
			err = errUnavailable
		}
		result.Error = err.Error()
		outbox.AppendGlobal(wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_Result{Result: result}}))
		return err
	}
	s.loop.Submit(func(h *hub.Hub) {
		h.SetNickname(slot, nickname)
		outbox.AppendGlobal(wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_Result{Result: result}}))
	})
	return nil
}

func (s *Server) write(ctx context.Context, conn *websocket.Conn, outbox *delivery.Outbox) error {
	var batch delivery.Batch
	timeout := time.Duration(s.config.Liveness.TimeoutSeconds * float64(time.Second))
	tick := time.Duration(float64(time.Second) / s.config.SendRateHz)
	var writeContext context.Context
	send := func(frame []byte) error {
		if err := conn.Write(writeContext, websocket.MessageBinary, frame); err != nil {
			return err
		}
		outbox.AddWritten(len(frame))
		return nil
	}
	// Wakes the writer for checkpoints of far peers deferred by their rate. It
	// fires a tick late, so a tick's batch usually carries them first and no
	// separate write is needed.
	deferred := time.NewTimer(time.Hour)
	deferred.Stop()
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-outbox.Ready():
		case <-deferred.C:
		}
		ok, next := outbox.Drain(&batch, time.Now())
		if !ok {
			if ctx.Err() == nil {
				return errBacklog
			}
			return ctx.Err()
		}
		if !next.IsZero() {
			deferred.Reset(time.Until(next) + tick)
		}
		if batch.Empty() {
			continue
		}
		var cancel context.CancelFunc
		writeContext, cancel = context.WithTimeout(ctx, timeout)
		err := batch.Write(send)
		cancel()
		if err != nil {
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
