#pragma once

#include <RE/Skyrim.h>

namespace SurvivalPolicy
{
    // Installs the remote-ownership actor update policy and the target-side
    // damage, kill, and magic firewall.
    void InstallHooks();
    // Registers hit and magic observation sinks. Requires Skyrim's data-loaded
    // state.
    void RegisterEventSinks();

    // Reasserts AI/combat suppression, survival, and dispels suppressed magic.
    void ApplyRemoteReplicaPolicy(RE::Actor& a_actor);
    // Clears local combat, alarm, and flee presentation. Main-loop thread only.
    void SanitizeRemoteCombatState(RE::Actor& a_actor);
}
