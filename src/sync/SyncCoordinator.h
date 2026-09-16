#pragma once

#include <RE/Skyrim.h>

namespace SyncCoordinator
{
	// The single synchronization entry point, called once per main-loop update
	// on the bound game thread. Drains network changes once, then runs the local
	// and remote stages in a fixed order.
	void OnFrame(RE::PlayerCharacter* a_player, float a_delta);
}
