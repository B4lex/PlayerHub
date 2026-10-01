package world

import "testing"

func TestFindAddressedPlayer(t *testing.T) {
	n := NewNames()
	n.Set(1, "Lydia")
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
		{"LYDIA  hi", 1, "hi", true},
		{"Lydia #2 hi", 1, "#2 hi", true},
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
}

func TestNameMovesToItsLatestHolder(t *testing.T) {
	n := NewNames()
	n.Set(1, "Lydia")
	// A session that connects again takes its name before the old connection
	// is forgotten.
	n.Set(2, "Lydia")
	n.Forget(1)
	if player, _, found := n.Find("lydia hi", 128); !found || player != 2 {
		t.Fatalf("found %d %v", player, found)
	}
}
