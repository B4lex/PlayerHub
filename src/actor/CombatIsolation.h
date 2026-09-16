#pragma once

#include <cstddef>

namespace CombatIsolation
{
	// Trampoline bytes InstallHooks needs: three entry detours of a 5-byte
	// gateway plus two 14-byte absolute jumps each.
	inline constexpr std::size_t kTrampolineSize = 3 * (5 + 2 * 14);

	// Keeps local actors from treating a remote actor as hostile or starting
	// combat with it, and vice versa. Requires kTrampolineSize trampoline bytes.
	void InstallHooks();
}
