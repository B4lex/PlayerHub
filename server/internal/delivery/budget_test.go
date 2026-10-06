package delivery

import (
	"testing"
	"time"

	"google.golang.org/protobuf/proto"

	"playerhub/server/internal/wire/pb"
)

func drainAt(t *testing.T, o *Outbox, now time.Time) (peers []uint32, ordered int) {
	t.Helper()
	var b Batch
	if ok, _ := o.Drain(&b, now); !ok {
		t.Fatal("drain refused")
	}
	err := b.Write(func(frame []byte) error {
		var m pb.ServerMessage
		if err := proto.Unmarshal(frame, &m); err != nil {
			return err
		}
		if states := m.GetPeerStates(); states != nil {
			for _, s := range states.States {
				peers = append(peers, s.Player)
			}
		} else {
			ordered++
		}
		return nil
	})
	if err != nil {
		t.Fatal(err)
	}
	return peers, ordered
}

func TestBudgetSpreadsCheckpointsWithoutStarving(t *testing.T) {
	start := time.Unix(1000, 0)
	size := len(entry(1, 1))
	budget := NewBudget(float64(size*4+messageOverheadBytes)*40/uplinkShare, 0)
	o := NewOutbox(64)
	o.UseBudget(budget)
	lastSeen := make(map[uint32]time.Time)
	now := start
	for tick := range 200 {
		for peer := uint32(1); peer <= 20; peer++ {
			o.SetCheckpoint(peer, uint64(tick+1), entry(peer, uint64(tick+1)), now)
		}
		peers, _ := drainAt(t, o, now)
		if tick >= 10 && len(peers) > 5 {
			t.Fatalf("tick %d wrote %d entries, over the budget", tick, len(peers))
		}
		for _, peer := range peers {
			lastSeen[peer] = now
		}
		now = now.Add(25 * time.Millisecond)
	}
	for peer := uint32(1); peer <= 20; peer++ {
		if gap := now.Sub(lastSeen[peer]); gap > time.Second {
			t.Errorf("peer %d waited %v", peer, gap)
		}
	}
}

func TestBudgetPrefersNearPeersAndSendsOrderedMessages(t *testing.T) {
	now := time.Unix(1000, 0)
	size := len(entry(1, 1))
	budget := NewBudget(float64(size*2+messageOverheadBytes)*40/uplinkShare, 0)
	o := NewOutbox(256)
	o.UseBudget(budget)
	for peer := uint32(1); peer <= 100; peer++ {
		o.SetClass(peer, 0, 0.125)
		o.SetCheckpoint(peer, 1, entry(peer, 1), now)
	}
	o.SetClass(200, 0, 1)
	o.SetCheckpoint(200, 1, entry(200, 1), now)
	peers, _ := drainAt(t, o, now.Add(25*time.Millisecond))
	if len(peers) == 0 || len(peers) > 50 || peers[0] != 200 {
		t.Fatalf("written %v; the near peer should lead a budgeted batch", peers)
	}

	for i := range 20 {
		o.AppendGlobal(action(1, uint64(i+1)))
	}
	_, ordered := drainAt(t, o, now.Add(50*time.Millisecond))
	if ordered != 20 {
		t.Fatalf("wrote %d of 20 ordered messages", ordered)
	}
	peers, _ = drainAt(t, o, now.Add(75*time.Millisecond))
	if len(peers) != 0 {
		t.Fatalf("checkpoints written while the ordered messages overdrew the allowance: %v", peers)
	}
}

func TestBudgetSharesTheUplink(t *testing.T) {
	budget := NewBudget(1000, 0)
	a, b := NewOutbox(4), NewOutbox(4)
	a.UseBudget(budget)
	b.UseBudget(budget)
	if share := budget.share(); share != 400 {
		t.Fatalf("share of two: %v", share)
	}
	b.Close()
	if share := budget.share(); share != 800 {
		t.Fatalf("share after one left: %v", share)
	}
	capped := NewBudget(1000, 100)
	NewOutbox(4).UseBudget(capped)
	if share := capped.share(); share != 100 {
		t.Fatalf("capped share: %v", share)
	}
}
