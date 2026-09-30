package world

import "testing"

func TestNameIndexes(t *testing.T) {
	n := NewNames()
	if got := n.Set(1, "Lydia"); got != 1 {
		t.Fatalf("first Lydia got %d", got)
	}
	if got := n.Set(2, "lydia"); got != 2 {
		t.Fatalf("second Lydia got %d", got)
	}
	if got := n.Set(3, "LYDIA"); got != 3 {
		t.Fatalf("third Lydia got %d", got)
	}
	n.Forget(2)
	if got := n.Set(4, "Lydia"); got != 2 {
		t.Fatalf("freed index not reused: %d", got)
	}
	if got := n.Set(3, "Lydia"); got != 3 {
		t.Fatalf("unchanged name renumbered to %d", got)
	}
	if got := n.Set(1, "Serana"); got != 1 {
		t.Fatalf("renamed player got %d", got)
	}
	if got := n.Index(9); got != 1 {
		t.Fatalf("unnamed player got %d", got)
	}
}
