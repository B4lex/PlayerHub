// Command server runs the Skyrim multiplayer server.
package main

import (
	"context"
	"errors"
	"flag"
	"log/slog"
	"net"
	"net/http"
	"net/http/pprof"
	"os"
	"os/signal"
	"syscall"
	"time"

	"skyrim-mp/server/internal/admin"
	"skyrim-mp/server/internal/config"
	"skyrim-mp/server/internal/delivery"
	"skyrim-mp/server/internal/hub"
	"skyrim-mp/server/internal/interest"
	"skyrim-mp/server/internal/loadtest"
	"skyrim-mp/server/internal/playback"
	"skyrim-mp/server/internal/recording"
	"skyrim-mp/server/internal/session"
)

func main() {
	configPath := flag.String("config", "", "TOML configuration file; defaults apply when omitted")
	listen := flag.String("listen", "", "address to listen on, overriding the configuration")
	verbose := flag.Bool("verbose", false, "log debug messages")
	profile := flag.Bool("pprof", false, "serve net/http/pprof under /debug/pprof/")
	flag.Parse()

	level := slog.LevelInfo
	if *verbose {
		level = slog.LevelDebug
	}
	log := slog.New(slog.NewTextHandler(os.Stderr, &slog.HandlerOptions{Level: level}))

	c, err := config.Load(*configPath)
	if err != nil {
		log.Error("invalid configuration", "error", err)
		os.Exit(2)
	}
	if *listen != "" {
		c.Listen = *listen
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if err := run(ctx, c, log, *profile); err != nil {
		log.Error("server stopped", "error", err)
		os.Exit(1)
	}
}

func run(ctx context.Context, c config.Config, log *slog.Logger, profile bool) error {
	// The feed is built from everything below, so changes reach it late.
	var feed *admin.Feed
	changed := func() {
		if feed != nil {
			feed.Changed()
		}
	}

	store := recording.NewStore(c.RecordingsDirectory, log)
	captures := recording.NewCaptures(store, log, changed)
	sessions := session.NewServer(nil, c, log)
	var entersPerSecond float64
	if c.Budget.Enabled {
		sessions.UseBudget(delivery.NewBudget(c.Budget.UplinkBytesPerSecond, c.Budget.PerRecipientBytesPerSecond))
		entersPerSecond = c.Budget.EntersPerSecond
	}
	h := hub.New(hub.Options{
		EntersPerSecond:  entersPerSecond,
		ActionsPerSecond: c.Limits.ActionsPerSecond,
		ActionBurst:      c.Limits.ActionBurst,
		Interest: interest.Config{
			EnterRadius:       c.Interest.EnterRadius,
			LeaveRadius:       c.Interest.LeaveRadius,
			MinDwellSeconds:   c.Interest.MinDwellSeconds,
			UpdateHz:          c.Interest.UpdateHz,
			MaxVisible:        c.Interest.MaxVisible,
			CapHysteresis:     c.Interest.CapHysteresis,
			NearDistance:      c.Interest.NearDistance,
			FarRateHz:         c.Interest.FarRateHz,
			MinimumViewRadius: c.Interest.MinimumViewRadius,
		},
		Recorder: captures,
		Logger:   log,
		OnChange: changed,
	})
	loop := hub.NewLoop(h, c.SendRateHz)
	loopContext, stopLoop := context.WithCancel(context.Background())
	defer stopLoop()
	go loop.Run(loopContext)

	playbacks := playback.NewManager(store, loop, log, changed)
	batches := loadtest.NewManager(store, loop, log, changed)
	adminContext := &admin.Context{
		Loop: loop, Store: store, Captures: captures, Playbacks: playbacks, Batches: batches,
		StartedAt: time.Now(), OnChange: changed,
	}
	feed = admin.NewFeed(adminContext.State)
	go feed.Run(loopContext)

	mux := http.NewServeMux()
	sessions.SetLoop(loop)
	mux.Handle("GET /ws", sessions)
	mux.HandleFunc("GET /health", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"status":"ok"}`))
	})
	adminContext.Routes(mux)
	mux.Handle("GET /api/events", feed)
	mux.Handle("GET /admin/", admin.UI(c.AdminUIDirectory))
	mux.Handle("GET /admin", http.RedirectHandler("/admin/", http.StatusMovedPermanently))
	if profile {
		mux.HandleFunc("/debug/pprof/", pprof.Index)
		mux.HandleFunc("/debug/pprof/profile", pprof.Profile)
	}

	server := &http.Server{
		Addr:              c.Listen,
		Handler:           mux,
		ReadHeaderTimeout: 10 * time.Second,
		// Shutdown does not close WebSocket or event-stream connections;
		// cancelling their request context does.
		BaseContext: func(net.Listener) context.Context { return ctx },
	}
	serveErr := make(chan error, 1)
	go func() { serveErr <- server.ListenAndServe() }()
	log.Info("listening", "address", c.Listen, "send_rate_hz", c.SendRateHz)

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
	err := server.Shutdown(shutdown)
	if errors.Is(err, context.DeadlineExceeded) {
		err = nil
	}
	return err
}
