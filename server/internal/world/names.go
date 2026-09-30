package world

import (
	"strconv"
	"strings"
)

// Names tells apart players with the same name. The first holder of a name
// gets index 1 and later ones the lowest index free among them. A player keeps
// its index while its name stays the same, so indexes are never renumbered
// when someone leaves. Names compare case-insensitively.
type Names struct {
	byPlayer map[uint32]nameIndex
	// Folded name to its holders by index.
	byName map[string]map[uint32]uint32
}

type nameIndex struct {
	folded string
	index  uint32
}

func NewNames() *Names {
	return &Names{byPlayer: make(map[uint32]nameIndex), byName: make(map[string]map[uint32]uint32)}
}

// Set records the player's name and returns its index.
func (n *Names) Set(player uint32, name string) uint32 {
	folded := strings.ToLower(name)
	if current, ok := n.byPlayer[player]; ok {
		if current.folded == folded {
			return current.index
		}
		n.Forget(player)
	}
	holders := n.byName[folded]
	if holders == nil {
		holders = make(map[uint32]uint32)
		n.byName[folded] = holders
	}
	index := uint32(1)
	for {
		if _, taken := holders[index]; !taken {
			break
		}
		index++
	}
	holders[index] = player
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
	entry, ok := n.byPlayer[player]
	if !ok {
		return
	}
	delete(n.byPlayer, player)
	holders := n.byName[entry.folded]
	delete(holders, entry.index)
	if len(holders) == 0 {
		delete(n.byName, entry.folded)
	}
}

// Find resolves a player addressed at the start of text: the longest known
// name followed by a space or the end, then optionally " #N" for index N
// (index 1 without it). It returns the player and the rest of the text
// without leading spaces. Names may contain spaces, so the longest match
// wins.
func (n *Names) Find(text string, maximumNameBytes int) (uint32, string, bool) {
	for end := len(text); end > 0; end = strings.LastIndexByte(text[:end], ' ') {
		if end > maximumNameBytes {
			continue
		}
		holders, ok := n.byName[strings.ToLower(text[:end])]
		if !ok {
			continue
		}
		rest := strings.TrimLeft(text[end:], " ")
		index := uint32(1)
		if tag, after, _ := strings.Cut(rest, " "); len(tag) > 1 && tag[0] == '#' {
			if parsed, err := strconv.ParseUint(tag[1:], 10, 32); err == nil {
				index, rest = uint32(parsed), strings.TrimLeft(after, " ")
			}
		}
		if player, ok := holders[index]; ok {
			return player, rest, true
		}
		return 0, "", false
	}
	return 0, "", false
}
