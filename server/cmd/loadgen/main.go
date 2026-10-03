// Command loadgen connects bots to a server as real WebSocket clients and
// measures peer-to-peer latency. Bots either replay a recording, spread over
// its timeline like an admin batch, or walk randomly in a square. Every bot
// takes a new session, named after the run so that runs against one server do
// not find their display names taken.
//
// Each bot sends the send time in microseconds since the Unix epoch as its
// checkpoint sequence, which is strictly rising and needs no wire field, so a
// receiver on the same host measures latency directly, across processes too.
package main

import (
	"context"
	"flag"
	"fmt"
	"math"
	"math/rand/v2"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"github.com/coder/websocket"
	"google.golang.org/protobuf/proto"

	"skyrim-mp/server/internal/loadtest"
	"skyrim-mp/server/internal/playback"
	"skyrim-mp/server/internal/recording"
	"skyrim-mp/server/internal/wire"
	"skyrim-mp/server/internal/wire/pb"
)

type options struct {
	url        string
	bots       int
	duration   time.Duration
	rampUp     time.Duration
	rateHz     float64
	spread     float64
	speed      float64
	recording  string
	seed       uint64
	report     time.Duration
	serverPID  int
	worldspace uint32
	idle       float64
	viewRadius uint
	near       float64
	// Part of every bot's display name.
	run string
}

func main() {
	var o options
	flag.StringVar(&o.url, "url", "https://8fa21dsv.lmfao.pp.ua/ws", "server WebSocket URL")
	flag.IntVar(&o.bots, "bots", 100, "number of bots")
	flag.DurationVar(&o.duration, "duration", 60*time.Second, "how long to run after the ramp-up")
	flag.DurationVar(&o.rampUp, "ramp-up", 10*time.Second, "time over which bots join")
	flag.Float64Var(&o.rateHz, "rate", 20, "checkpoints per second per walking bot")
	flag.Float64Var(&o.spread, "spread", 20000, "walking bots stay within ±spread units: 20000 is dense, 100000 sparse")
	flag.Float64Var(&o.speed, "speed", 350, "walking speed in units per second")
	flag.StringVar(&o.recording, "recording", "", "replay this version 3 recording instead of walking")
	flag.Uint64Var(&o.seed, "seed", 1, "layout seed")
	flag.DurationVar(&o.report, "report", 5*time.Second, "report interval")
	flag.IntVar(&o.serverPID, "server-pid", 0, "report this local process's CPU use (Linux)")
	flag.Float64Var(&o.idle, "idle", 0, "share of walking bots that stand still, repeating one checkpoint")
	flag.UintVar(&o.viewRadius, "view-radius", 0, "view radius the bots ask for; 0 is the server's")
	flag.Float64Var(&o.near, "near", 4096, "peers within this distance are reported as near")
	flag.Parse()
	o.worldspace = 60
	o.run = fmt.Sprintf("%06x", rand.Uint32()&0xFFFFFF)

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if err := run(ctx, o); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
}

func run(ctx context.Context, o options) error {
	var r *recording.Recording
	var plans []loadtest.BotPlan
	if o.recording != "" {
		var err error
		if r, err = recording.Read(o.recording); err != nil {
			return err
		}
		plans = loadtest.PlanBots(o.bots, r.DurationMs(), o.seed, 0.5, 0, o.rampUp.Seconds())
	}

	stats := newStats()
	ctx, cancel := context.WithTimeout(ctx, o.rampUp+o.duration)
	defer cancel()
	var wg sync.WaitGroup
	started := time.Now()
	for i := range o.bots {
		delay := time.Duration(float64(o.rampUp) * float64(i) / float64(o.bots))
		wg.Add(1)
		go func() {
			defer wg.Done()
			select {
			case <-ctx.Done():
				return
			case <-time.After(time.Until(started.Add(delay))):
			}
			b := &bot{o: o, index: i, stats: stats}
			var err error
			if r != nil {
				err = b.replay(ctx, r, plans[i])
			} else {
				err = b.walk(ctx)
			}
			if err != nil && ctx.Err() == nil {
				stats.failed.Add(1)
				fmt.Fprintf(os.Stderr, "bot %d: %v\n", i+1, err)
			}
		}()
	}

	ticker := time.NewTicker(o.report)
	defer ticker.Stop()
	cpu := newCPUSampler(o.serverPID)
	var total, totalNear histogram
	for {
		select {
		case <-ctx.Done():
			wg.Wait()
			window, seconds := stats.take()
			total.merge(&window.latency)
			totalNear.merge(&window.near)
			fmt.Println(stats.line(window, seconds, cpu.sample()))
			fmt.Printf("total: latency p50 %s p99 %s max %s over %d entries; near p50 %s p99 %s over %d\n",
				total.quantile(0.5), total.quantile(0.99), total.max(), total.count,
				totalNear.quantile(0.5), totalNear.quantile(0.99), totalNear.count)
			return nil
		case <-ticker.C:
			window, seconds := stats.take()
			total.merge(&window.latency)
			totalNear.merge(&window.near)
			fmt.Println(stats.line(window, seconds, cpu.sample()))
		}
	}
}

type bot struct {
	o     options
	index int
	stats *stats
	conn  *websocket.Conn
	mu    sync.Mutex
	// The last sequence sent, and the virtual player's number it replaced.
	lastSent    uint64
	lastVirtual uint64
	// The bot's own last position, to tell near peers from far ones.
	x, y atomic.Int32
}

// displayName names the bot's session after its kind, the run, and its
// number, within the display name rules for up to 99999 bots.
func (b *bot) displayName(kind string) string {
	return fmt.Sprintf("%s%s_%d", kind, b.o.run, b.index+1)
}

func (b *bot) connect(ctx context.Context, displayName string) error {
	conn, _, err := websocket.Dial(ctx, b.o.url, &websocket.DialOptions{CompressionMode: websocket.CompressionDisabled})
	if err != nil {
		return err
	}
	conn.SetReadLimit(1 << 22)
	b.conn = conn
	b.stats.connected.Add(1)
	if err := b.send(ctx, &pb.ClientMessage{Message: &pb.ClientMessage_Hello{Hello: &pb.Hello{
		ProtocolVersion: uint32(pb.Protocol_PROTOCOL_VERSION), ViewRadius: uint32(b.o.viewRadius),
		DisplayName: displayName}}}); err != nil {
		return err
	}
	go b.receive(ctx)
	return nil
}

func (b *bot) send(ctx context.Context, m *pb.ClientMessage) error {
	data, err := proto.Marshal(m)
	if err != nil {
		return err
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.conn.Write(ctx, websocket.MessageBinary, data)
}

// stamp gives a checkpoint the current time as its sequence.
func (b *bot) stamp(c *pb.Checkpoint) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.lastVirtual = c.Sequence
	b.lastSent = max(uint64(time.Now().UnixMicro()), b.lastSent+1)
	c.Sequence = b.lastSent
	c.Time = wire.SampleTime(time.Now())
	b.x.Store(c.X)
	b.y.Store(c.Y)
}

func (b *bot) receive(ctx context.Context) {
	defer b.stats.connected.Add(-1)
	var window, near, gaps histogram
	// When each peer was last updated, to find the longest wait.
	lastUpdate := make(map[uint32]time.Time)
	flush := time.Now()
	for {
		_, data, err := b.conn.Read(ctx)
		if err != nil {
			return
		}
		now := uint64(time.Now().UnixMicro())
		b.stats.bytes.Add(int64(len(data)))
		var m pb.ServerMessage
		if proto.Unmarshal(data, &m) != nil {
			continue
		}
		received := time.Now()
		if enter := m.GetPeerEnter(); enter != nil {
			lastUpdate[enter.Slot] = received
		}
		if leave := m.GetPeerLeave(); leave != nil {
			delete(lastUpdate, leave.Slot)
		}
		for _, state := range m.GetPeerStates().GetStates() {
			// Only stamped sequences are times; virtual players count from 1.
			if last, ok := lastUpdate[state.Player]; ok {
				gaps.add(received.Sub(last))
			}
			lastUpdate[state.Player] = received
			if state.Sequence > 1_000_000_000_000_000 && state.Sequence <= now {
				latency := time.Duration(now-state.Sequence) * time.Microsecond
				window.add(latency)
				if math.Hypot(float64(state.X-b.x.Load()), float64(state.Y-b.y.Load())) <= b.o.near {
					near.add(latency)
				}
			}
		}
		b.stats.entries.Add(int64(len(m.GetPeerStates().GetStates())))
		if time.Since(flush) > 200*time.Millisecond {
			b.stats.merge(&window, &near, &gaps)
			window, near, gaps = histogram{}, histogram{}, histogram{}
			flush = time.Now()
		}
	}
}

func (b *bot) enter(ctx context.Context) error {
	messages := []*pb.ClientMessage{
		{Message: &pb.ClientMessage_Appearance{Appearance: &pb.Appearance{
			Revision: 1, Race: &pb.FormRef{Plugin: "Skyrim.esm", FormId: 0x13746}, Weight: 50, Height: 1,
			BodyTint: &pb.Color{R: 200, G: 170, B: 150}, FaceMorphs: make([]float32, 19), FaceParts: make([]int32, 4)}}},
		{Message: &pb.ClientMessage_Enter{Enter: &pb.Enter{}}},
		{Message: &pb.ClientMessage_Context{Context: &pb.Context{WorldspaceId: b.o.worldspace, CellId: 1, GraphEpoch: 1}}},
	}
	for _, m := range messages {
		if err := b.send(ctx, m); err != nil {
			return err
		}
	}
	return nil
}

func (b *bot) walk(ctx context.Context) error {
	if err := b.connect(ctx, b.displayName("W")); err != nil {
		return err
	}
	defer b.conn.CloseNow()
	random := rand.New(rand.NewPCG(b.o.seed, uint64(b.index)))
	x := (random.Float64()*2 - 1) * b.o.spread
	y := (random.Float64()*2 - 1) * b.o.spread
	heading := random.Float64() * 2 * math.Pi
	if err := b.enter(ctx); err != nil {
		return err
	}
	interval := time.Duration(float64(time.Second) / b.o.rateHz)
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	idle := float64(b.index%100) < b.o.idle*100
	for {
		select {
		case <-ctx.Done():
			return nil
		case <-ticker.C:
		}
		sample := wire.Sample{X: x, Y: y, Heading: heading, MovementState: pb.MovementState_MOVEMENT_STATE_IDLE}
		if !idle {
			// Turn a little each step and away from the edge of the square.
			heading += (random.Float64() - 0.5) * 0.3
			step := b.o.speed * interval.Seconds()
			nx, ny := x+math.Sin(heading)*step, y+math.Cos(heading)*step
			if math.Abs(nx) > b.o.spread || math.Abs(ny) > b.o.spread {
				heading += math.Pi
				nx, ny = x, y
			}
			x, y = nx, ny
			sample = wire.Sample{X: x, Y: y, Heading: heading, Speed: b.o.speed,
				MovementState: pb.MovementState_MOVEMENT_STATE_RUNNING}
		}
		c := wire.Quantize(sample, 0)
		b.stamp(c)
		if err := b.send(ctx, &pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: c}}); err != nil {
			return err
		}
		b.stats.sent.Add(1)
	}
}

// replayPlayer sends a playback session's messages over the bot's
// connection, stamping checkpoints with the send time.
type replayPlayer struct {
	b   *bot
	ctx context.Context
	err atomic.Pointer[error]
}

func (p *replayPlayer) Send(m *pb.ClientMessage) {
	switch body := m.Message.(type) {
	case *pb.ClientMessage_Checkpoint:
		p.b.stamp(body.Checkpoint)
		p.b.stats.sent.Add(1)
	case *pb.ClientMessage_Action:
		p.b.mu.Lock()
		if body.Action.CheckpointSequence == p.b.lastVirtual {
			body.Action.CheckpointSequence = p.b.lastSent
		}
		p.b.mu.Unlock()
	}
	if err := p.b.send(p.ctx, m); err != nil {
		p.err.CompareAndSwap(nil, &err)
	}
}

func (p *replayPlayer) Leave() {}

func (b *bot) replay(ctx context.Context, r *recording.Recording, plan loadtest.BotPlan) error {
	if err := b.connect(ctx, b.displayName("R")); err != nil {
		return err
	}
	defer b.conn.CloseNow()
	player := &replayPlayer{b: b, ctx: ctx}
	// The server names the bot after its session, whatever was recorded.
	session, err := playback.NewSession(strconv.Itoa(b.index), "", r, player, playback.Options{
		Autoplay: true, Rate: plan.Rate, Loop: true, StartMs: plan.StartMs})
	if err != nil {
		return err
	}
	session.Start()
	<-ctx.Done()
	session.Close()
	if err := player.err.Load(); err != nil && ctx.Err() == nil {
		return *err
	}
	return nil
}

type stats struct {
	connected atomic.Int64
	failed    atomic.Int64
	sent      atomic.Int64
	entries   atomic.Int64
	bytes     atomic.Int64

	mu      sync.Mutex
	latency histogram
	near    histogram
	gaps    histogram
	since   time.Time
}

type window struct {
	sent, entries, bytes int64
	latency, near, gaps  histogram
}

func newStats() *stats {
	return &stats{since: time.Now()}
}

func (s *stats) merge(all, near, gaps *histogram) {
	s.mu.Lock()
	s.latency.merge(all)
	s.near.merge(near)
	s.gaps.merge(gaps)
	s.mu.Unlock()
}

func (s *stats) take() (window, float64) {
	s.mu.Lock()
	defer s.mu.Unlock()
	w := window{sent: s.sent.Swap(0), entries: s.entries.Swap(0), bytes: s.bytes.Swap(0), latency: s.latency,
		near: s.near, gaps: s.gaps}
	s.latency, s.near, s.gaps = histogram{}, histogram{}, histogram{}
	seconds := time.Since(s.since).Seconds()
	s.since = time.Now()
	return w, seconds
}

func (s *stats) line(w window, seconds float64, cpu string) string {
	parts := []string{
		fmt.Sprintf("bots %d (failed %d)", s.connected.Load(), s.failed.Load()),
		fmt.Sprintf("sent %.0f/s", float64(w.sent)/seconds),
		fmt.Sprintf("received %.0f entries/s %.2f MB/s", float64(w.entries)/seconds, float64(w.bytes)/seconds/1e6),
		fmt.Sprintf("latency p50 %s p99 %s max %s", w.latency.quantile(0.5), w.latency.quantile(0.99), w.latency.max()),
		fmt.Sprintf("near p99 %s", w.near.quantile(0.99)),
		// The histogram saturates at 2 s.
		fmt.Sprintf("update gap p99 %s max %s", w.gaps.quantile(0.99), w.gaps.max()),
	}
	if cpu != "" {
		parts = append(parts, "server CPU "+cpu)
	}
	return strings.Join(parts, " | ")
}

// histogram counts latencies in 0.1 ms buckets up to 2 s.
type histogram struct {
	buckets [20001]uint32
	count   uint64
}

const bucketWidth = 100 * time.Microsecond

func (h *histogram) add(d time.Duration) {
	h.buckets[min(int(d/bucketWidth), len(h.buckets)-1)]++
	h.count++
}

func (h *histogram) merge(other *histogram) {
	for i, n := range other.buckets {
		h.buckets[i] += n
	}
	h.count += other.count
}

func (h *histogram) quantile(q float64) time.Duration {
	if h.count == 0 {
		return 0
	}
	target := uint64(math.Ceil(q * float64(h.count)))
	var seen uint64
	for i, n := range h.buckets {
		seen += uint64(n)
		if seen >= target {
			return time.Duration(i+1) * bucketWidth
		}
	}
	return h.max()
}

func (h *histogram) max() time.Duration {
	for i := len(h.buckets) - 1; i >= 0; i-- {
		if h.buckets[i] > 0 {
			return time.Duration(i+1) * bucketWidth
		}
	}
	return 0
}

// cpuSampler reads a process's user and system time from /proc.
type cpuSampler struct {
	pid   int
	ticks float64
	at    time.Time
}

// Linux reports process times in clock ticks of 1/100 s on every common
// configuration.
const clockTicksPerSecond = 100

func newCPUSampler(pid int) *cpuSampler {
	s := &cpuSampler{pid: pid}
	s.ticks, _ = s.read()
	s.at = time.Now()
	return s
}

func (s *cpuSampler) read() (float64, bool) {
	if s.pid == 0 {
		return 0, false
	}
	data, err := os.ReadFile(fmt.Sprintf("/proc/%d/stat", s.pid))
	if err != nil {
		return 0, false
	}
	// Fields after the parenthesized command name; utime and stime are the
	// 14th and 15th fields of the line.
	fields := strings.Fields(string(data[strings.LastIndexByte(string(data), ')')+1:]))
	if len(fields) < 13 {
		return 0, false
	}
	utime, _ := strconv.ParseFloat(fields[11], 64)
	stime, _ := strconv.ParseFloat(fields[12], 64)
	return utime + stime, true
}

func (s *cpuSampler) sample() string {
	ticks, ok := s.read()
	if !ok {
		return ""
	}
	now := time.Now()
	share := (ticks - s.ticks) / clockTicksPerSecond / now.Sub(s.at).Seconds()
	s.ticks, s.at = ticks, now
	return fmt.Sprintf("%.0f%%", share*100)
}
