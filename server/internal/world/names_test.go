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

func TestFindAddressedPlayer(t *testing.T) {
	n := NewNames()
	n.Set(1, "Lydia")
	n.Set(2, "lydia")
	n.Set(3, "Ulfric")
	n.Set(4, "Ulfric Stormcloak")
	n.Set(5, "Serana")
	n.Set(5, "Valerica")
	cases := []struct {
		text   string
		player uint32
		rest   string
		found  bool
	}{
		{"Lydia hello there", 1, "hello there", true},
		{"LYDIA #2  hi", 2, "hi", true},
		{"lydia #3 hi", 0, "", false},
		{"Lydia #two", 1, "#two", true},
		{"Lydia", 1, "", true},
		{"Ulfric Stormcloak for Skyrim", 4, "for Skyrim", true},
		{"Ulfric Stormcloaks", 3, "Stormcloaks", true},
		{"Serana hi", 0, "", false},
		{"Valerica hi", 5, "hi", true},
		{"Lydiahi", 0, "", false},
	}
	for _, c := range cases {
		player, rest, found := n.Find(c.text, 128)
		if player != c.player || rest != c.rest || found != c.found {
			t.Errorf("Find(%q) = %d %q %v, want %d %q %v", c.text, player, rest, found, c.player, c.rest, c.found)
		}
	}
	if _, _, found := n.Find("Ulfric Stormcloak hi", 10); !found {
		t.Error("a shorter name within the bound was not found")
	}
	if player, _, _ := n.Find("Ulfric Stormcloak hi", 10); player != 3 {
		t.Errorf("a name beyond the bound matched player %d", player)
	}
	n.Forget(1)
	if _, _, found := n.Find("Lydia hi", 128); found {
		t.Error("a forgotten player was found")
	}
	if got := n.Set(6, "Lydia"); got != 1 {
		t.Fatalf("freed index not reused: %d", got)
	}
}
