#pragma once

#include <cstddef>

namespace CombatIsolation
{
	// Trampoline bytes InstallHooks needs: four entry detours, each a gateway of
	// the moved prologue plus two 14-byte absolute jumps, and one call detour.
	inline constexpr std::size_t kTrampolineSize =
		3 * (5 + 2 * 14) + (6 + 2 * 14) + 14;

	// Keeps local actors from treating a remote actor as hostile or starting
	// combat with it, and vice versa, and drops the melee and projectile hits a
	// remote actor's replicated attacks would land on the local world. Requires
	// kTrampolineSize trampoline bytes.
	void InstallHooks();
}
