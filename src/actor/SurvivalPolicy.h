#pragma once

#include <RE/Skyrim.h>

namespace SurvivalPolicy
{
    void InstallHooks();
    void RegisterEventSinks();

    void ApplyRemoteReplicaPolicy(RE::Actor& a_actor);

    void AssertDoNothingPackage(RE::Actor& a_actor);
    void SanitizeRemoteCombatState(RE::Actor& a_actor, bool a_mayResetPackage);
}
