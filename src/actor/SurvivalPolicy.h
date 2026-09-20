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

    // Gives the actor the do-nothing package and installs it as the run-once
    // package, which outranks the packages quest and scene systems select for a
    // bystander. Every path which hands a remote actor back to ordinary
    // synchronization uses this rather than InitiateDoNothingPackage alone.
    // Main-loop thread only.
    void AssertDoNothingPackage(RE::Actor& a_actor);
    // Clears local combat, alarm, and flee presentation. With a_mayResetPackage
    // it also takes back any package the engine selected for the actor; the
    // caller clears it until the actor's first do-nothing package is installed.
    // Main-loop thread only.
    void SanitizeRemoteCombatState(RE::Actor& a_actor, bool a_mayResetPackage);
}
