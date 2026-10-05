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
	"net/netip"
	"net/url"
	"sync"
	"time"

	"github.com/coder/websocket"
	"google.golang.org/protobuf/proto"

	"skyrim-mp/server/internal/admission"
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
	// StatusDisplayNameTaken closes a connection whose Hello asks for a display
	// name another session holds.
	StatusDisplayNameTaken websocket.StatusCode = 4004
	// StatusDisplayNameInvalid closes a connection whose Hello has no display
	// name or one that cannot be a display name.
	StatusDisplayNameInvalid websocket.StatusCode = 4005
	// StatusReplaced closes a connection whose session connected again.
	StatusReplaced websocket.StatusCode = 4006
	// StatusServerFull closes a connection whose session would exceed
	// limits.max_players.
	StatusServerFull websocket.StatusCode = 4007
	// StatusTooManyNewSessions closes a connection whose hello needs a new
	// session when its address took limits.new_sessions_per_address_per_hour.
	StatusTooManyNewSessions websocket.StatusCode = 4008
)

// How often connected sessions are marked used and expired ones deleted.
const maintenanceInterval = time.Hour

type Server struct {
	loop   *hub.Loop
	store  *auth.Store
	config config.Config
	log    *slog.Logger
	// Nil without an egress budget.
	budget  *delivery.Budget
	gate    *admission.Gate
	trusted []netip.Prefix
	// Joins refused before a WebSocket exists, which a flood would repeat.
	refusals *rejectionLog

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
	// config.Load validated the prefixes.
	trusted, _ := c.TrustedProxyPrefixes()
	gate := admission.NewGate(admission.Limits{
		MaxPlayers:                   c.Limits.MaxPlayers,
		MaxPendingHellos:             c.Limits.MaxPendingHellos,
		NewSessionsPerAddressPerHour: c.Limits.NewSessionsPerAddressPerHour,
	})
	return &Server{loop: loop, store: store, config: c, log: log, gate: gate, trusted: trusted,
		refusals: newRejectionLog(log, "refused connections"), connections: make(map[uint32]*connection)}
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
	if !playerOrigin(r) {
		s.log.Debug("browser refused", "address", r.RemoteAddr, "origin", r.Header.Get("Origin"))
		http.Error(w, "browsers cannot join", http.StatusForbidden)
		return
	}
	if !s.gate.BeginHello() {
		s.refusals.note("too many connections waiting for their hello")
		http.Error(w, "too many players are joining; try again", http.StatusServiceUnavailable)
		return
	}
	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{
		CompressionMode: websocket.CompressionDisabled,
		// playerOrigin checked the origin. Behind a proxy the plugin's origin
		// names the public host and port, which need not match Host.
		InsecureSkipVerify: true,
	})
	if err != nil {
		s.gate.EndHello()
		return
	}
	conn.SetReadLimit(readLimitBytes)
	s.serve(r.Context(), conn, admission.ClientAddress(r, s.trusted))
}

// playerOrigin refuses WebSocket upgrades from web pages, so a page cannot
// make its visitors' browsers join. A browser always sends the page's origin,
// whose scheme is never ws or wss. IXWebSocket, the plugin's transport, sends
// ws://host:port or wss://host:port, and Go tools send none.
func playerOrigin(r *http.Request) bool {
	for _, origin := range r.Header.Values("Origin") {
		u, err := url.Parse(origin)
		if err != nil || (u.Scheme != "ws" && u.Scheme != "wss") {
			return false
		}
	}
	return true
}

func (s *Server) serve(parent context.Context, conn *websocket.Conn, address netip.Addr) {
	ctx, cancel := context.WithCancel(parent)
	defer cancel()
	defer conn.CloseNow()

	hello, identity, err := s.join(ctx, conn, address)
	if err != nil {
		s.log.Info("player refused", "address", address, "display_name", hello.GetDisplayName(), "error", err)
		return
	}
	defer s.gate.Leave(identity.Session)

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
		h.SetVanished(slot, hello.GetVanished())
		s.mu.Lock()
		replaced = s.connections[replacedSlot]
		s.connections[slot] = current
		s.mu.Unlock()
	}) {
		conn.Close(websocket.StatusGoingAway, "server stopping")
		return
	}
	log := s.log.With("slot", slot, "session", identity.Session)
	log.Info("player connected", "address", address, "display_name", identity.DisplayName, "new_session", identity.Token != nil)
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

var errServerFull = errors.New("server full")

// join reads the hello and admits its session. A refused connection is closed
// with the status that tells the client why; the hello is nil when none came.
func (s *Server) join(ctx context.Context, conn *websocket.Conn, address netip.Addr) (*pb.Hello, hub.Identity, error) {
	defer s.gate.EndHello()
	hello, err := s.readHello(ctx, conn)
	if err != nil {
		return nil, hub.Identity{}, err
	}
	// Spares a full server the session it would issue and then delete.
	if len(hello.GetSessionToken()) == 0 && s.gate.Full() {
		conn.Close(StatusServerFull, errServerFull.Error())
		return hello, hub.Identity{}, errServerFull
	}
	identity, err := s.authenticate(conn, hello, address)
	if err != nil {
		return hello, hub.Identity{}, err
	}
	if !s.gate.Admit(identity.Session) {
		conn.Close(StatusServerFull, errServerFull.Error())
		if identity.Token != nil {
			if _, err := s.store.Abandon(identity.Session); err != nil {
				s.log.Error("cannot delete the unused session", "error", err)
			}
		}
		return hello, hub.Identity{}, errServerFull
	}
	return hello, identity, nil
}

// authenticate resumes or issues the session the hello asks for. A refused
// connection is closed with the status that tells the client why.
func (s *Server) authenticate(conn *websocket.Conn, hello *pb.Hello, address netip.Addr) (hub.Identity, error) {
	if err := validate.Hello(hello); err != nil {
		if errors.Is(err, validate.ErrDisplayName) {
			conn.Close(StatusDisplayNameInvalid, err.Error())
		} else {
			conn.Close(websocket.StatusPolicyViolation, err.Error())
		}
		return hub.Identity{}, err
	}
	now := time.Now()
	mayIssue := func() bool { return s.gate.MayIssue(address, now) }
	session, issued, err := s.store.Authenticate(hello.GetSessionToken(), hello.GetDisplayName(), now, mayIssue)
	switch {
	case errors.Is(err, auth.ErrDisplayNameTaken):
		conn.Close(StatusDisplayNameTaken, err.Error())
		return hub.Identity{}, err
	case errors.Is(err, auth.ErrIssueRefused):
		conn.Close(StatusTooManyNewSessions, err.Error())
		return hub.Identity{}, err
	case err != nil:
		s.log.Error("session store failed", "error", err)
		conn.Close(websocket.StatusInternalError, "session store unavailable")
		return hub.Identity{}, err
	}
	return hub.Identity{Session: session.ID, DisplayName: session.DisplayName, Token: issued}, nil
}

// settle ends a connection's use of its session. A client that never
// received the welcome of a new session cannot present its token, and the
// session would keep the display name from its own player until it expired,
// so it is deleted, unless the token came back on another connection
// meanwhile.
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
		s.gate.Prune(now)
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
)

func (s *Server) read(ctx context.Context, c *connection, slot uint32, outbox *delivery.Outbox, log *slog.Logger) error {
	var rate limit.TokenBucket
	rejections := newRejectionLog(log, "rejected player messages")
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
