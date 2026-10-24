package session

import (
	"context"
	"errors"
	"fmt"
	"log/slog"
	"net/http"
	"net/netip"
	"net/url"
	"slices"
	"sync"
	"time"

	"github.com/coder/websocket"
	"google.golang.org/protobuf/proto"

	"playerhub/server/internal/admission"
	"playerhub/server/internal/auth"
	"playerhub/server/internal/config"
	"playerhub/server/internal/delivery"
	"playerhub/server/internal/hub"
	"playerhub/server/internal/limit"
	"playerhub/server/internal/validate"
	"playerhub/server/internal/wire"
	"playerhub/server/internal/wire/pb"
)

const readLimitBytes = 20 * 1024

const (
	StatusProtocolMismatch   websocket.StatusCode = 4001
	StatusBacklog            websocket.StatusCode = 4002
	StatusRateLimited        websocket.StatusCode = 4003
	StatusDisplayNameTaken   websocket.StatusCode = 4004
	StatusDisplayNameInvalid websocket.StatusCode = 4005
	StatusReplaced           websocket.StatusCode = 4006
	StatusServerFull         websocket.StatusCode = 4007
	StatusTooManyNewSessions websocket.StatusCode = 4008
	StatusAddressFull        websocket.StatusCode = 4009
	StatusBanned             websocket.StatusCode = 4010
	StatusKicked             websocket.StatusCode = 4011
)

const maintenanceInterval = time.Hour

type Server struct {
	loop     *hub.Loop
	store    *auth.Store
	config   config.Config
	log      *slog.Logger
	budget   *delivery.Budget
	gate     *admission.Gate
	trusted  []netip.Prefix
	refusals *rejectionLog

	mu sync.Mutex
	// Set on the hub goroutine as a client connects, so a session that connects again finds
	// the connection it replaces.
	connections map[uint32]*connection
}

type connection struct {
	conn        *websocket.Conn
	cancel      context.CancelFunc
	session     uint64
	displayName string
	address     netip.Addr
	// Reader goroutine only.
	heard bool
}

func NewServer(loop *hub.Loop, store *auth.Store, c config.Config, log *slog.Logger) *Server {
	// config.Load validated the prefixes.
	trusted, _ := c.TrustedProxyPrefixes()
	gate := admission.NewGate(admission.Limits{
		MaxPlayers:                    c.Limits.MaxPlayers,
		MaxPlayersPerAddress:          c.Limits.MaxPlayersPerAddress,
		MaxPendingHellos:              c.Limits.MaxPendingHellos,
		MaxPendingHellosPerAddress:    c.Limits.MaxPendingHellosPerAddress,
		NewSessionsPerAddressPerMonth: c.Limits.NewSessionsPerAddressPerMonth,
	})
	return &Server{loop: loop, store: store, config: c, log: log, gate: gate, trusted: trusted,
		refusals: newRejectionLog(log, "refused connections"), connections: make(map[uint32]*connection)}
}

func (s *Server) UseBudget(b *delivery.Budget) {
	s.budget = b
}

func (s *Server) SetLoop(loop *hub.Loop) {
	s.loop = loop
}

func (s *Server) ServeHTTP(w http.ResponseWriter, r *http.Request) {
	if !playerOrigin(r) {
		s.log.Debug("browser refused", "address", r.RemoteAddr, "origin", r.Header.Get("Origin"))
		http.Error(w, "browsers cannot join", http.StatusForbidden)
		return
	}
	address := admission.ClientAddress(r, s.trusted)
	if err := s.gate.BeginHello(address); err != nil {
		s.refusals.note(err.Error())
		if errors.Is(err, admission.ErrAddressPendingHellos) {
			http.Error(w, "too many connections from your address are joining; try again", http.StatusTooManyRequests)
		} else {
			http.Error(w, "too many players are joining; try again", http.StatusServiceUnavailable)
		}
		return
	}
	conn, err := websocket.Accept(w, r, &websocket.AcceptOptions{
		CompressionMode: websocket.CompressionDisabled,
		// playerOrigin checked the origin. Behind a proxy the plugin's origin
		// names the public host and port, which need not match Host.
		InsecureSkipVerify: true,
	})
	if err != nil {
		s.gate.EndHello(address)
		return
	}
	conn.SetReadLimit(readLimitBytes)
	s.serve(r.Context(), conn, address)
}

// A browser always sends the page's origin, whose scheme is never ws or wss.
// IXWebSocket sends ws://host:port or wss://host:port, and Go tools send none.
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
	defer s.gate.Leave(identity.Session, address)

	outbox := delivery.NewOutbox(s.config.Limits.OrderedBacklog)
	if s.budget != nil {
		outbox.UseBudget(s.budget)
	}
	current := &connection{conn: conn, cancel: cancel, session: identity.Session, displayName: identity.DisplayName,
		address: address}
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
		go replaced.close(StatusReplaced, "session connected elsewhere")
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
	// A stopping server closes the store; Maintain marked the session used within the last
	// interval.
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

func (s *Server) join(ctx context.Context, conn *websocket.Conn, address netip.Addr) (*pb.Hello, hub.Identity, error) {
	defer s.gate.EndHello(address)
	hello, err := s.readHello(ctx, conn)
	if err != nil {
		return nil, hub.Identity{}, err
	}
	// Before the admission checks, so a banned player learns of the ban also on a full server.
	if banned, err := s.store.AddressBanned(admission.AddressKey(address)); err != nil || banned {
		if err == nil {
			err = errAddressBanned
			conn.Close(StatusBanned, "banned")
		} else {
			s.log.Error("session store failed", "error", err)
			conn.Close(websocket.StatusInternalError, "session store unavailable")
		}
		return hello, hub.Identity{}, err
	}
	if len(hello.GetSessionToken()) == 0 {
		if err := s.gate.CheckNew(address); err != nil {
			refuseAdmission(conn, err)
			return hello, hub.Identity{}, err
		}
	}
	identity, err := s.authenticate(conn, hello, address)
	if err != nil {
		return hello, hub.Identity{}, err
	}
	if err := s.gate.Admit(identity.Session, address); err != nil {
		refuseAdmission(conn, err)
		if identity.Token != nil {
			if _, err := s.store.Abandon(identity.Session); err != nil {
				s.log.Error("cannot delete the unused session", "error", err)
			}
		}
		return hello, hub.Identity{}, err
	}
	return hello, identity, nil
}

func refuseAdmission(conn *websocket.Conn, err error) {
	status := StatusServerFull
	if errors.Is(err, admission.ErrAddressFull) {
		status = StatusAddressFull
	}
	conn.Close(status, err.Error())
}

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
	case errors.Is(err, auth.ErrBanned):
		conn.Close(StatusBanned, err.Error())
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

// Closing waits for the peer, which may be gone, so callers run it on a goroutine of its own.
func (c *connection) close(status websocket.StatusCode, reason string) {
	c.conn.Close(status, reason)
	c.cancel()
}

var (
	ErrNotConnected       = errors.New("player is not connected")
	ErrAddressNotBannable = errors.New("the player's address is loopback, unspecified, or a trusted proxy; " +
		"behind a proxy, list it in trusted_proxies")
	errAddressBanned = errors.New("address banned")
)

func (s *Server) connection(slot uint32) *connection {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.connections[slot]
}

func (s *Server) Address(slot uint32) (netip.Addr, bool) {
	c := s.connection(slot)
	if c == nil {
		return netip.Addr{}, false
	}
	return c.address, true
}

// Kick leaves the player's choice to connect off, so the game does not reconnect by itself.
func (s *Server) Kick(slot uint32) bool {
	c := s.connection(slot)
	if c == nil {
		return false
	}
	s.log.Info("player kicked", "session", c.session, "display_name", c.displayName)
	go c.close(StatusKicked, "kicked")
	return true
}

// Ban closes the player's connection, and with the address every other one from it.
func (s *Server) Ban(slot uint32, withAddress bool, now time.Time) (auth.Ban, error) {
	c := s.connection(slot)
	if c == nil {
		return auth.Ban{}, ErrNotConnected
	}
	ban := auth.Ban{Session: c.session, DisplayName: c.displayName, CreatedAt: now}
	if withAddress {
		// Without a forwarded address every player behind the proxy shares the proxy's.
		if !c.address.IsValid() || c.address.IsLoopback() || c.address.IsUnspecified() ||
			slices.ContainsFunc(s.trusted, func(p netip.Prefix) bool { return p.Contains(c.address) }) {
			return auth.Ban{}, ErrAddressNotBannable
		}
		ban.Address = admission.AddressKey(c.address)
	}
	ban, err := s.store.Ban(ban)
	if err != nil {
		return auth.Ban{}, err
	}
	s.log.Info("player banned", "ban", ban.ID, "session", ban.Session, "display_name", ban.DisplayName,
		"address", ban.Address)
	s.mu.Lock()
	var closing []*connection
	for _, other := range s.connections {
		if other == c || (ban.Address.IsValid() && admission.AddressKey(other.address) == ban.Address) {
			closing = append(closing, other)
		}
	}
	s.mu.Unlock()
	for _, other := range closing {
		go other.close(StatusBanned, "banned")
	}
	return ban, nil
}

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
			outbox.AppendGlobal(wire.Marshal(&pb.ServerMessage{Message: &pb.ServerMessage_Pong{
				Pong: &pb.Pong{RequestId: ping.GetRequestId()}}}))
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
