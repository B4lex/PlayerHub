package world

import "strings"

// Names tells apart players with the same name. The first holder of a name
// gets index 1 and later ones the lowest index free among them. A player keeps
// its index while its name stays the same, so indexes are never renumbered
// when someone leaves. Names compare case-insensitively.
type Names struct {
	byPlayer map[uint32]nameIndex
}

type nameIndex struct {
	folded string
	index  uint32
}

func NewNames() *Names {
	return &Names{byPlayer: make(map[uint32]nameIndex)}
}

// Set records the player's name and returns its index.
func (n *Names) Set(player uint32, name string) uint32 {
	folded := strings.ToLower(name)
	if current, ok := n.byPlayer[player]; ok && current.folded == folded {
		return current.index
	}
	taken := make(map[uint32]struct{})
	for other, entry := range n.byPlayer {
		if other != player && entry.folded == folded {
			taken[entry.index] = struct{}{}
		}
	}
	index := uint32(1)
	for {
		if _, ok := taken[index]; !ok {
			break
		}
		index++
	}
	n.byPlayer[player] = nameIndex{folded, index}
	return index
}

// Index returns the player's index, 1 before it has a name.
func (n *Names) Index(player uint32) uint32 {
	if entry, ok := n.byPlayer[player]; ok {
		return entry.index
	}
	return 1
}

func (n *Names) Forget(player uint32) {
	delete(n.byPlayer, player)
}
