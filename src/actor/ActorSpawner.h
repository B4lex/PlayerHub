#pragma once

#include "actor/RemoteActor.h"
#include "protocol/MovementState.h"

#include <RE/Skyrim.h>

#include <optional>

namespace ActorSpawner
{
    // These functions operate on Skyrim objects and must run on the bound main-loop
    // thread. Network callbacks queue plain data for that frame hook to drain.
    [[nodiscard]] std::optional<RemoteActor> Spawn(RE::TESNPC* a_npcBase, const RE::NiPoint3& a_targetPosition,
        float a_targetHeading, Multiplayer::AnimationState a_animationState, float a_speed = 250.0F,
        float a_stoppingDistance = 1.0F);

    // Takes the reference out of the world and out of every later save: a deleted
    // reference keeps its change form, and that record names an actor base no save
    // can carry.
    void Remove(RemoteActor& a_remote);

    // Whether Spawn can currently place an actor at the position: an exterior
    // position needs its cell loaded; an interior uses the local player's cell.
    [[nodiscard]] bool IsSpawnLocationLoaded(const RE::NiPoint3& a_position);

    // Excludes a live actor's reference from a save while one is being written,
    // and restores it afterwards. Disable and SetDelete do not keep a reference
    // out of a save, so a retired one is excluded by Remove instead.
    void SetExcludedFromSaves(const RemoteActor& a_remote, bool a_excluded);
}
