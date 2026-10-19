package main

import (
	"context"
	"math"
	"strconv"
	"strings"
	"time"

	"github.com/coder/websocket"
	"google.golang.org/protobuf/proto"

	"playerhub/server/internal/adminclient"
	"playerhub/server/internal/wire"
	"playerhub/server/internal/wire/pb"
)

const (
	tickInterval       = 50 * time.Millisecond
	maximumStepSeconds = 0.25
	// Farther than this, or in another space, the bot jumps next to the target instead of running.
	teleportDistance = 2500.0
	// The bot starts running once the target is this much farther than --distance.
	followSlack = 150.0
	// Past the server's default trade.keep_radius (1000).
	leaveDistance = 1500.0
	// Admin state lags the relay, so it only places the target while the relay is quiet.
	peerQuietSeconds = 1.5
)

type mode int

const (
	modeFollow mode = iota
	modeStay
	modeLeave
)

type space struct {
	cell, worldspace uint32
}

type vec struct {
	x, y, z float64
}

type target struct {
	slot     uint32
	name     string
	located  bool
	space    space
	pos      vec
	heading  float64
	speed    float64
	peerSeen time.Time
	// Names already listed while waiting for a choice between several players.
	listed string
}

type bot struct {
	o    options
	ctx  context.Context
	conn *websocket.Conn
	slot uint32
	err  error

	target   target
	mode     mode
	placed   bool
	space    space
	pos      vec
	heading  float64
	chasing  bool
	sequence uint64

	trade      trade
	offer      []*pb.TradeItem
	skips      []skipRule
	mirror     bool
	autoAnswer bool
	autoAccept bool
}

func newBot(ctx context.Context, o options, conn *websocket.Conn) *bot {
	b := &bot{o: o, ctx: ctx, conn: conn, autoAnswer: o.autoAnswer, autoAccept: o.autoAccept}
	if o.kit {
		b.offer = testKit()
	}
	return b
}

func (b *bot) send(m *pb.ClientMessage) error {
	data, err := proto.Marshal(m)
	if err != nil {
		return err
	}
	return b.conn.Write(b.ctx, websocket.MessageBinary, data)
}

// post sends from the loop, where a failed write ends the run after the current event.
func (b *bot) post(m *pb.ClientMessage) {
	if err := b.send(m); err != nil && b.err == nil {
		b.err = err
	}
}

func (b *bot) enter() error {
	for _, m := range []*pb.ClientMessage{
		{Message: &pb.ClientMessage_Appearance{Appearance: &pb.Appearance{
			Revision: 1, Name: b.o.name, Level: 10, Race: &pb.FormRef{Plugin: "Skyrim.esm", FormId: 0x13746},
			Weight: 50, Height: 1, BodyTint: &pb.Color{R: 200, G: 170, B: 150},
			FaceMorphs: make([]float32, 19), FaceParts: make([]int32, 4)}}},
		{Message: &pb.ClientMessage_Enter{Enter: &pb.Enter{}}},
	} {
		if err := b.send(m); err != nil {
			return err
		}
	}
	return nil
}

func (b *bot) locate(players []adminclient.Player) {
	t := &b.target
	var found *adminclient.Player
	var candidates []*adminclient.Player
	for i := range players {
		p := &players[i]
		if p.Virtual || p.ID == strconv.FormatUint(uint64(b.slot), 10) {
			continue
		}
		if t.slot != 0 && p.ID == strconv.FormatUint(uint64(t.slot), 10) {
			found = p
			break
		}
		if t.slot == 0 && p.InWorld && p.Position != nil &&
			(b.o.target == "" || strings.EqualFold(p.Name, b.o.target)) {
			candidates = append(candidates, p)
		}
	}
	if t.slot == 0 {
		switch {
		case len(candidates) == 1:
			found = candidates[0]
			slot, _ := strconv.ParseUint(found.ID, 10, 32)
			*t = target{slot: uint32(slot), name: found.Name}
			say("following %s (slot %d)", t.name, t.slot)
		case len(candidates) > 1:
			names := make([]string, len(candidates))
			for i, c := range candidates {
				names[i] = c.Name
			}
			if listed := strings.Join(names, ", "); listed != t.listed {
				t.listed = listed
				say("several players are in the world (%s); choose one with target <name>", listed)
			}
		}
	}
	if found == nil {
		if t.slot != 0 {
			say("%s disconnected; standing still until a target joins", t.name)
			*t = target{}
		}
		return
	}
	if !found.InWorld || found.Position == nil || found.CellID == nil || found.WorldspaceID == nil {
		t.located = false
		return
	}
	if time.Since(t.peerSeen).Seconds() < peerQuietSeconds {
		return
	}
	t.located = true
	t.space = space{*found.CellID, *found.WorldspaceID}
	t.pos = vec{found.Position.X, found.Position.Y, found.Position.Z}
}

func (b *bot) chooseTarget(name string) {
	b.o.target = name
	b.target = target{}
	b.placed = false
}

func (b *bot) observe(c *pb.Checkpoint) {
	t := &b.target
	x, y, z := wire.Position(c)
	t.pos = vec{x, y, z}
	t.heading = float64(c.GetHeading()) / wire.HeadingSteps * 2 * math.Pi
	t.speed = float64(c.GetSpeed())
	t.peerSeen = time.Now()
	t.located = true
}

func (b *bot) observeContext(c *pb.Context) {
	b.target.space = space{c.GetCellId(), c.GetWorldspaceId()}
}

func (b *bot) handlePeer(m *pb.ServerMessage) bool {
	t := &b.target
	switch body := m.Message.(type) {
	case *pb.ServerMessage_PeerEnter:
		if body.PeerEnter.GetSlot() == t.slot && t.slot != 0 {
			if c := body.PeerEnter.GetContext(); c != nil {
				b.observeContext(c)
			}
			if s := body.PeerEnter.GetState(); s != nil {
				b.observe(s)
			}
		}
	case *pb.ServerMessage_PeerContext:
		if body.PeerContext.GetSlot() == t.slot && t.slot != 0 {
			b.observeContext(body.PeerContext.GetContext())
		}
	case *pb.ServerMessage_PeerStates:
		for _, s := range body.PeerStates.GetStates() {
			if s.GetPlayer() == t.slot && t.slot != 0 {
				b.observe(s)
			}
		}
	case *pb.ServerMessage_PeerLeave:
		if body.PeerLeave.GetSlot() == t.slot {
			t.peerSeen = time.Time{}
		}
	default:
		return false
	}
	return true
}

func (b *bot) tick(dt float64) error {
	b.move(dt)
	if !b.placed {
		return b.err
	}
	sample := wire.Sample{X: b.pos.x, Y: b.pos.y, Z: b.pos.z, Heading: b.heading,
		MovementState: pb.MovementState_MOVEMENT_STATE_IDLE}
	if b.chasing || b.mode == modeLeave {
		sample.Speed = max(b.o.speed, b.target.speed)
		sample.MovementState = pb.MovementState_MOVEMENT_STATE_RUNNING
	}
	c := wire.Quantize(sample, 0)
	b.sequence = max(uint64(time.Now().UnixMicro()), b.sequence+1)
	c.Sequence = b.sequence
	c.Time = wire.SampleTime(time.Now())
	b.post(&pb.ClientMessage{Message: &pb.ClientMessage_Checkpoint{Checkpoint: c}})
	return b.err
}

func (b *bot) move(dt float64) {
	t := &b.target
	if !t.located {
		b.chasing = false
		return
	}
	if !b.placed || b.space != t.space {
		b.teleport()
		return
	}
	dx, dy := t.pos.x-b.pos.x, t.pos.y-b.pos.y
	d := math.Hypot(dx, dy)
	if d < 1 {
		dx, dy, d = 0, 1, 1
	}
	step := max(b.o.speed, t.speed) * dt
	switch b.mode {
	case modeFollow:
		if d > teleportDistance {
			b.teleport()
			return
		}
		b.chasing = d > b.o.distance+followSlack || b.chasing && d > b.o.distance
		if b.chasing {
			step = min(step, d-b.o.distance)
			b.pos = vec{b.pos.x + dx/d*step, b.pos.y + dy/d*step, t.pos.z}
		}
	case modeLeave:
		if d >= leaveDistance {
			b.mode = modeStay
			say("stopped %.0f units away", d)
			break
		}
		b.pos = vec{b.pos.x - dx/d*step, b.pos.y - dy/d*step, t.pos.z}
		b.heading = math.Atan2(-dx, -dy)
		return
	case modeStay:
		b.chasing = false
	}
	b.heading = math.Atan2(dx, dy)
}

func (b *bot) teleport() {
	t := &b.target
	b.pos = vec{t.pos.x + math.Sin(t.heading)*b.o.distance, t.pos.y + math.Cos(t.heading)*b.o.distance, t.pos.z}
	b.heading = t.heading + math.Pi
	b.chasing = false
	if !b.placed || b.space != t.space {
		b.space = t.space
		b.post(&pb.ClientMessage{Message: &pb.ClientMessage_Context{Context: &pb.Context{
			CellId: b.space.cell, WorldspaceId: b.space.worldspace, GraphEpoch: 1}}})
	}
	if !b.placed {
		say("standing in front of %s", t.name)
	}
	b.placed = true
	if b.mode == modeLeave {
		b.mode = modeStay
	}
}

func (b *bot) distanceToTarget() float64 {
	return math.Hypot(b.target.pos.x-b.pos.x, b.target.pos.y-b.pos.y)
}
