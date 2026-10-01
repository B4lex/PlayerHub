package world

import (
	"strings"

	"skyrim-mp/server/internal/validate"
)

// Names finds connected players by nickname, which sessions keep unique, for
// whispers.
type Names struct {
	byPlayer map[uint32]string
	// Nickname key to its holder.
	byName map[string]uint32
}

func NewNames() *Names {
	return &Names{byPlayer: make(map[uint32]string), byName: make(map[string]uint32)}
}

// Set records the player's nickname in place of its previous one.
func (n *Names) Set(player uint32, name string) {
	n.Forget(player)
	key := validate.NicknameKey(name)
	n.byPlayer[player] = key
	n.byName[key] = player
}

func (n *Names) Forget(player uint32) {
	key, ok := n.byPlayer[player]
	if !ok {
		return
	}
	delete(n.byPlayer, player)
	if n.byName[key] == player {
		delete(n.byName, key)
	}
}

// Find resolves a player addressed at the start of text: the longest known
// name followed by a space or the end. It returns the player and the rest of
// the text without leading spaces. Names may contain spaces, so the longest
// match wins.
func (n *Names) Find(text string, maximumNameBytes int) (uint32, string, bool) {
	for end := len(text); end > 0; end = strings.LastIndexByte(text[:end], ' ') {
		if end > maximumNameBytes {
			continue
		}
		if player, ok := n.byName[validate.NicknameKey(text[:end])]; ok {
			return player, strings.TrimLeft(text[end:], " "), true
		}
	}
	return 0, "", false
}
