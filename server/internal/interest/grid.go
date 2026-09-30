package interest

import "math"

type cellKey struct {
	space  Space
	cx, cy int32
}

// Grid indexes placed players by space and square cell, so candidates come
// from the 3×3 cells around a viewer instead of every player. The cell size
// is at least the leave radius, so every pair that matters is in those cells.
type Grid struct {
	size  float64
	cells map[cellKey]map[uint32]struct{}
	at    map[uint32]cellKey
}

func NewGrid(size float64) *Grid {
	return &Grid{size: size, cells: make(map[cellKey]map[uint32]struct{}), at: make(map[uint32]cellKey)}
}

func (g *Grid) key(v *View) cellKey {
	return cellKey{v.Space, int32(math.Floor(v.X / g.size)), int32(math.Floor(v.Y / g.size))}
}

// Update places or moves a player. It costs a map lookup when the player
// stays in its cell.
func (g *Grid) Update(v *View) {
	key := g.key(v)
	if previous, ok := g.at[v.Slot]; ok {
		if previous == key {
			return
		}
		g.remove(v.Slot, previous)
	}
	cell := g.cells[key]
	if cell == nil {
		cell = make(map[uint32]struct{})
		g.cells[key] = cell
	}
	cell[v.Slot] = struct{}{}
	g.at[v.Slot] = key
}

func (g *Grid) Remove(slot uint32) {
	if key, ok := g.at[slot]; ok {
		g.remove(slot, key)
	}
}

func (g *Grid) remove(slot uint32, key cellKey) {
	delete(g.at, slot)
	cell := g.cells[key]
	delete(cell, slot)
	if len(cell) == 0 {
		delete(g.cells, key)
	}
}

// Near calls f for every player in the 3×3 cells around v, v itself
// included.
func (g *Grid) Near(v *View, f func(slot uint32)) {
	center := g.key(v)
	for dx := int32(-1); dx <= 1; dx++ {
		for dy := int32(-1); dy <= 1; dy++ {
			for slot := range g.cells[cellKey{center.space, center.cx + dx, center.cy + dy}] {
				f(slot)
			}
		}
	}
}
