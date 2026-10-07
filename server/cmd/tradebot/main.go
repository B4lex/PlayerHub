package main

import (
	"bufio"
	"context"
	"errors"
	"flag"
	"fmt"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"github.com/coder/websocket"
	"google.golang.org/protobuf/proto"

	"playerhub/server/internal/adminclient"
	"playerhub/server/internal/wire/pb"
)

type options struct {
	url        string
	admin      string
	name       string
	target     string
	sessions   string
	distance   float64
	speed      float64
	viewRadius uint
	kit        bool
	autoAnswer bool
	autoAccept bool
}

func main() {
	var o options
	flag.StringVar(&o.url, "url", "http://localhost:3000/ws", "server WebSocket URL")
	flag.StringVar(&o.admin, "admin", "http://127.0.0.1:3001", "server admin address: issues the bot's session and shows where the target is")
	flag.StringVar(&o.name, "name", "TradeBot", "the bot's display name")
	flag.StringVar(&o.target, "target", "", "display name of the player to follow; empty follows the only other player in the world")
	flag.StringVar(&o.sessions, "sessions", "", "file that keeps the bot's session between runs; default in the user cache directory, per --url host")
	flag.Float64Var(&o.distance, "distance", 150, "distance the bot keeps from the target, in game units")
	flag.Float64Var(&o.speed, "speed", 350, "the bot's least running speed in units per second")
	flag.UintVar(&o.viewRadius, "view-radius", 0, "view radius the bot asks for; 0 is the server's")
	flag.BoolVar(&o.kit, "kit", false, "start with the test kit in the bot's offer")
	flag.BoolVar(&o.autoAnswer, "auto-answer", true, "accept invitations at once")
	flag.BoolVar(&o.autoAccept, "auto-accept", true, "accept the exchange once the target accepted it")
	flag.Parse()

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if err := run(ctx, o); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func run(ctx context.Context, o options) error {
	token, err := session(ctx, o)
	if err != nil {
		return err
	}
	conn, _, err := websocket.Dial(ctx, o.url, &websocket.DialOptions{CompressionMode: websocket.CompressionDisabled})
	if err != nil {
		return err
	}
	defer conn.CloseNow()
	conn.SetReadLimit(1 << 22)

	b := newBot(ctx, o, conn)
	if err := b.send(&pb.ClientMessage{Message: &pb.ClientMessage_Hello{Hello: &pb.Hello{
		ProtocolVersion: uint32(pb.Protocol_PROTOCOL_VERSION), ViewRadius: uint32(o.viewRadius),
		DisplayName: o.name, SessionToken: token}}}); err != nil {
		return err
	}
	welcome, err := b.readWelcome()
	if err != nil {
		return err
	}
	b.slot = welcome.GetSlot()
	say("connected as %s, slot %d; type help for commands", welcome.GetDisplayName(), b.slot)
	if err := b.enter(); err != nil {
		return err
	}

	messages := make(chan *pb.ServerMessage, 256)
	readErr := make(chan error, 1)
	go func() {
		for {
			_, data, err := conn.Read(ctx)
			if err != nil {
				readErr <- err
				return
			}
			var m pb.ServerMessage
			if proto.Unmarshal(data, &m) == nil {
				messages <- &m
			}
		}
	}()
	lines := make(chan string)
	go func() {
		scanner := bufio.NewScanner(os.Stdin)
		for scanner.Scan() {
			lines <- scanner.Text()
		}
	}()
	players := make(chan []adminclient.Player, 1)
	go pollPlayers(ctx, o.admin, players)

	ticker := time.NewTicker(tickInterval)
	defer ticker.Stop()
	last := time.Now()
	for {
		select {
		case <-ctx.Done():
			conn.Close(websocket.StatusNormalClosure, "")
			return nil
		case err := <-readErr:
			if ctx.Err() != nil {
				return nil
			}
			return fmt.Errorf("disconnected: %w", err)
		case m := <-messages:
			b.handle(m)
		case line := <-lines:
			if !b.command(line) {
				conn.Close(websocket.StatusNormalClosure, "")
				return nil
			}
		case list := <-players:
			b.locate(list)
		case now := <-ticker.C:
			dt := min(now.Sub(last).Seconds(), maximumStepSeconds)
			last = now
			if err := b.tick(dt); err != nil {
				return err
			}
		}
		if b.err != nil {
			return b.err
		}
	}
}

func session(ctx context.Context, o options) ([]byte, error) {
	if o.admin == "" {
		return nil, errors.New("--admin is required: the bot finds the target through it")
	}
	path := o.sessions
	if path == "" {
		var err error
		if path, err = adminclient.DefaultSessionsPath("tradebot", o.url); err != nil {
			return nil, err
		}
	}
	file, err := adminclient.LoadSessions(path)
	if err != nil {
		return nil, err
	}
	client := &http.Client{Timeout: 10 * time.Second}
	token, err := adminclient.IssueSession(ctx, client, o.admin, o.name, file.Tokens[o.name])
	if errors.Is(err, adminclient.ErrDisplayNameTaken) {
		return nil, fmt.Errorf("%s is taken by a session %s does not hold; pick another --name", o.name, path)
	}
	if err != nil {
		return nil, fmt.Errorf("cannot get a session for %s from %s: %w", o.name, o.admin, err)
	}
	file.Tokens[o.name] = token
	return token, adminclient.SaveSessions(path, file)
}

func pollPlayers(ctx context.Context, admin string, out chan<- []adminclient.Player) {
	client := &http.Client{Timeout: 2 * time.Second}
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	reported := false
	for {
		list, err := adminclient.Players(ctx, client, admin)
		switch {
		case err != nil && !reported && ctx.Err() == nil:
			say("cannot read the players from %s: %v", admin, err)
			reported = true
		case err == nil:
			reported = false
			select {
			case out <- list:
			default:
			}
		}
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
		}
	}
}

func (b *bot) readWelcome() (*pb.Welcome, error) {
	_, data, err := b.conn.Read(b.ctx)
	if err != nil {
		var closeErr websocket.CloseError
		if errors.As(err, &closeErr) {
			return nil, fmt.Errorf("the server refused the bot: %d %s", closeErr.Code, closeErr.Reason)
		}
		return nil, err
	}
	var m pb.ServerMessage
	if err := proto.Unmarshal(data, &m); err != nil {
		return nil, err
	}
	welcome := m.GetWelcome()
	if welcome == nil {
		return nil, errors.New("the server's first message is not a welcome")
	}
	return welcome, nil
}

func say(format string, args ...any) {
	fmt.Printf(time.Now().Format("15:04:05 ")+format+"\n", args...)
}
