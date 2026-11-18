package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log/slog"
	"net"
	"net/http"
	"net/http/pprof"
	"os"
	"os/signal"
	"syscall"
	"time"

	"playerhub/server/internal/admin"
	"playerhub/server/internal/adminauth"
	"playerhub/server/internal/auth"
	"playerhub/server/internal/config"
	"playerhub/server/internal/delivery"
	"playerhub/server/internal/hub"
	"playerhub/server/internal/interest"
	"playerhub/server/internal/loadtest"
	"playerhub/server/internal/metrics"
	"playerhub/server/internal/playback"
	"playerhub/server/internal/recording"
	"playerhub/server/internal/session"
	"playerhub/server/internal/voice"
	"playerhub/server/internal/wire/pb"
	"playerhub/server/internal/world"
)

const defaultConfigPath = "config.toml"

func main() {
	configPath := flag.String("config", "", "TOML configuration file; "+defaultConfigPath+
		" in the working directory when omitted, or defaults if there is none")
	listen := flag.String("listen", "", "address to listen on, overriding the configuration")
	adminListen := flag.String("admin-listen", "", "address of the admin panel and API, overriding the configuration")
	verbose := flag.Bool("verbose", false, "log debug messages")
	profile := flag.Bool("pprof", false, "serve net/http/pprof under /debug/pprof/ on the admin address")
	flag.Parse()

	level := slog.LevelInfo
	if *verbose {
		level = slog.LevelDebug
	}
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level}))

	path := *configPath
	if path == "" {
		if _, err := os.Stat(defaultConfigPath); err == nil {
			path = defaultConfigPath
		}
	}
	c, err := config.Load(path)
	if err != nil {
		log.Error("invalid configuration", "file", path, "error", err)
		os.Exit(2)
	}
	if path != "" {
		log.Info("configuration read", "file", path)
	}
	if *listen != "" {
		c.Listen = *listen
	}
	if *adminListen != "" {
		c.AdminListen = *adminListen
	}
	if err := c.Validate(); err != nil {
		log.Error("invalid configuration", "error", err)
		os.Exit(2)
	}
	if *profile && c.AdminListen == "" {
		log.Error("invalid configuration", "error", "--pprof needs admin_listen")
		os.Exit(2)
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if err := run(ctx, c, log, *profile); err != nil {
		log.Error("server stopped", "error", err)
		os.Exit(1)
	}
}

func run(ctx context.Context, c config.Config, log *slog.Logger, profile bool) error {
	var feed *admin.Feed
	changed := func() {
		if feed != nil {
			feed.Changed()
		}
	}

	identities, err := auth.Open(c.Sessions.Database, time.Duration(c.Sessions.LifetimeDays*24*float64(time.Hour)))
	if err != nil {
		return fmt.Errorf("cannot open the session database %q: %w", c.Sessions.Database, err)
	}
	defer identities.Close()

	metricsStore, err := metrics.Open(c.Metrics.Database, time.Duration(c.Metrics.RetentionDays*24*float64(time.Hour)))
	if err != nil {
		return fmt.Errorf("cannot open the metrics database %q: %w", c.Metrics.Database, err)
	}
	defer metricsStore.Close()
	if metricsStore.Fresh() {
		created, err := identities.CreatedTimes()
		if err == nil {
			err = metricsStore.AddPlayers(created)
		}
		if err != nil {
			return fmt.Errorf("cannot copy the sessions' first connections into the metrics database: %w", err)
		}
	}
	recorder, err := metrics.NewRecorder(metricsStore, log)
	if err != nil {
		return fmt.Errorf("cannot read the metrics database %q: %w", c.Metrics.Database, err)
	}

	store := recording.NewStore(c.RecordingsDirectory, log)
	captures := recording.NewCaptures(store, log, changed)
	sessions := session.NewServer(nil, identities, c, log)
	sessions.UseMetrics(recorder)
	var entersPerSecond float64
	if c.Budget.Enabled {
		sessions.UseBudget(delivery.NewBudget(c.Budget.UplinkBytesPerSecond, c.Budget.PerRecipientBytesPerSecond))
		entersPerSecond = c.Budget.EntersPerSecond
	}
	h := hub.New(hub.Options{
		EntersPerSecond:    entersPerSecond,
		ChatNearbyRadius:   c.Chat.NearbyRadius,
		TradeRequestRadius: c.Trade.RequestRadius,
		TradeKeepRadius:    c.Trade.KeepRadius,
		TradeInviteSeconds: c.Trade.InviteSeconds,
		VoiceRadius:        c.Voice.Radius,
		ActionsPerSecond:   c.Limits.ActionsPerSecond,
		ActionBurst:        c.Limits.ActionBurst,
		Interest: interest.Config{
			EnterRadius:       c.Interest.EnterRadius,
			LeaveRadius:       c.Interest.LeaveRadius,
			MinDwellSeconds:   c.Interest.MinDwellSeconds,
			UpdateHz:          c.Interest.UpdateHz,
			MaxVisible:        c.Interest.MaxVisible,
			MaxPosed:          c.Interest.MaxPosed,
			CapHysteresis:     c.Interest.CapHysteresis,
			NearDistance:      c.Interest.NearDistance,
			FarRateHz:         c.Interest.FarRateHz,
			MinimumViewRadius: c.Interest.MinimumViewRadius,
		},
		Recorder: captures,
		Logger:   log,
		OnChange: changed,
		OnGlobalChat: func(speaker *pb.PlayerIdentity, text string) {
			if feed != nil {
				feed.Chat(speaker.GetSlot(), speaker.GetName(), text)
			}
		},
	})
	loop := hub.NewLoop(h, c.SendRateHz)
	loopContext, stopLoop := context.WithCancel(context.Background())
	defer stopLoop()
	go loop.Run(loopContext)

	playbacks := playback.NewManager(store, loop, log, changed)
	batches := loadtest.NewManager(store, loop, log, changed)
	adminContext := &admin.Context{
		Loop: loop, Store: store, Captures: captures, Playbacks: playbacks, Batches: batches, Sessions: identities,
		Connections: sessions, Metrics: metricsStore, StartedAt: time.Now(), OnChange: changed,
	}
	if c.Budget.Enabled {
		adminContext.UplinkBytesPerSecond = c.Budget.UplinkBytesPerSecond
	}
	feed = admin.NewFeed(adminContext.State)
	go feed.Run(loopContext)
	go sessions.Maintain(loopContext)
	recorderContext, stopRecorder := context.WithCancel(context.Background())
	recorderDone := make(chan struct{})
	go func() {
		defer close(recorderDone)
		recorder.Run(recorderContext, func() (metrics.Gauges, bool) { return probe(loop, sessions) })
	}()
	// After the servers, so connections they close are recorded as ended before the rest are.
	defer func() {
		stopRecorder()
		<-recorderDone
	}()
	if c.Voice.Enabled {
		relay, err := voice.Listen(voice.Config{Listen: c.Voice.Listen, PublicHost: c.Voice.PublicHost,
			PublicPort: c.Voice.PublicPort, Radius: c.Voice.Radius, PacketsPerSecond: c.Voice.PacketsPerSecond,
			PacketBurst: c.Voice.PacketBurst}, log)
		if err != nil {
			return fmt.Errorf("cannot listen for voice datagrams on %q: %w", c.Voice.Listen, err)
		}
		go relay.Run(loopContext)
		sessions.UseVoice(relay)
		log.Info("relaying voice", "address", relay.LocalAddr(), "radius", c.Voice.Radius)
	}

	game := http.NewServeMux()
	sessions.SetLoop(loop)
	if err := sessions.PublishRoles(); err != nil {
		return fmt.Errorf("cannot read the roles: %w", err)
	}
	game.Handle("GET /ws", sessions)
	game.HandleFunc("GET /health", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"status":"ok"}`))
	})
	servers := []*http.Server{newServer(ctx, c.Listen, game)}

	if c.AdminListen != "" {
		panel := http.NewServeMux()
		adminContext.Routes(panel)
		panel.Handle("GET /api/events", feed)
		panel.Handle("GET /admin/", admin.UI(c.AdminUIDirectory))
		panel.Handle("GET /admin", http.RedirectHandler("/admin/", http.StatusMovedPermanently))
		if profile {
			panel.HandleFunc("/debug/pprof/", pprof.Index)
			panel.HandleFunc("/debug/pprof/profile", pprof.Profile)
		}
		trusted, _ := c.TrustedProxyPrefixes()
		gate := adminauth.New(c.AdminPassword(), trusted, log)
		servers = append(servers, newServer(ctx, c.AdminListen, admin.Guard(gate.Wrap(panel), c.AdminHosts)))
		if c.AdminPasswordHash == "" {
			log.Warn("the admin panel has no password; everyone who reaches admin_listen controls the server",
				"admin_address", c.AdminListen)
		}
	}

	serveErr := make(chan error, len(servers))
	for _, server := range servers {
		go func() { serveErr <- server.ListenAndServe() }()
	}
	log.Info("listening", "address", c.Listen, "admin_address", c.AdminListen, "send_rate_hz", c.SendRateHz)

	select {
	case err := <-serveErr:
		return err
	case <-ctx.Done():
	}
	captures.StopAll("shutdown")
	playbacks.CloseAll()
	batches.CloseAll()
	shutdown, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	var errs []error
	for _, server := range servers {
		if err := server.Shutdown(shutdown); !errors.Is(err, context.DeadlineExceeded) {
			errs = append(errs, err)
		}
	}
	return errors.Join(errs...)
}

func probe(loop *hub.Loop, sessions *session.Server) (metrics.Gauges, bool) {
	var g metrics.Gauges
	ok := loop.Call(func(h *hub.Hub) {
		h.Players(func(p *world.Player, _ int, _ *delivery.Outbox) {
			if p.Kind == world.KindClient {
				g.Players++
				if p.InWorld {
					g.InWorld++
				}
			}
		})
	})
	g.BytesIn, g.BytesOut = sessions.Traffic()
	g.LoopBusy, g.LoopWorst = loop.Load()
	return g, ok
}

func newServer(ctx context.Context, address string, handler http.Handler) *http.Server {
	return &http.Server{
		Addr:              address,
		Handler:           handler,
		ReadHeaderTimeout: 10 * time.Second,
		// Shutdown does not close WebSocket or event-stream connections;
		// cancelling their request context does.
		BaseContext: func(net.Listener) context.Context { return ctx },
	}
}
