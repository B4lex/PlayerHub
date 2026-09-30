// Command server runs the Skyrim multiplayer server.
package main

import (
	"context"
	"errors"
	"flag"
	"log/slog"
	"net"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"skyrim-mp/server/internal/config"
	"skyrim-mp/server/internal/hub"
	"skyrim-mp/server/internal/session"
)

func main() {
	configPath := flag.String("config", "", "TOML configuration file; defaults apply when omitted")
	listen := flag.String("listen", "", "address to listen on, overriding the configuration")
	verbose := flag.Bool("verbose", false, "log debug messages")
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
	if err := run(ctx, c, log); err != nil {
		log.Error("server stopped", "error", err)
		os.Exit(1)
	}
}

func run(ctx context.Context, c config.Config, log *slog.Logger) error {
	h := hub.New(hub.Options{
		ActionsPerSecond: c.Limits.ActionsPerSecond,
		ActionBurst:      c.Limits.ActionBurst,
		Logger:           log,
	})
	loop := hub.NewLoop(h, c.SendRateHz)
	loopContext, stopLoop := context.WithCancel(context.Background())
	defer stopLoop()
	go loop.Run(loopContext)

	mux := http.NewServeMux()
	mux.Handle("GET /ws", session.NewServer(loop, c, log))
	mux.HandleFunc("GET /health", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"status":"ok"}`))
	})

	server := &http.Server{
		Addr:              c.Listen,
		Handler:           mux,
		ReadHeaderTimeout: 10 * time.Second,
		// Shutdown does not close WebSocket connections; cancelling their
		// request context does.
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
	shutdown, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	err := server.Shutdown(shutdown)
	if errors.Is(err, context.DeadlineExceeded) {
		err = nil
	}
	return err
}
