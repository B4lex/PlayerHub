#pragma once

#include "actor/RemoteActor.h"

#include <RE/Skyrim.h>

#include <optional>

namespace ActorSpawner
{
    [[nodiscard]] std::optional<RemoteActor> Spawn(RE::TESNPC* a_npcBase, const RE::NiPoint3& a_targetPosition,
        float a_targetHeading, float a_speed, float a_stoppingDistance);

    void Remove(RemoteActor& a_remote);

    [[nodiscard]] bool IsSpawnLocationLoaded(const RE::NiPoint3& a_position);

    // Disable and SetDelete do not keep a reference out of a save.
    void SetExcludedFromSaves(const RemoteActor& a_remote, bool a_excluded);
}
