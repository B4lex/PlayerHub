#pragma once

#include "protocol/AppearanceState.h"

#include <RE/Skyrim.h>

#include <span>
#include <vector>

namespace RaceMenuBodyMorphs
{
    // The player's morphs summed per name over every key, sorted by name.
    [[nodiscard]] std::vector<Multiplayer::AppearanceBodyMorph> Capture(RE::Actor& a_player);

    // Replaces every morph on the actor, other mods' included, with a_morphs, also
    // when a_morphs is empty.
    void Apply(RE::Actor& a_actor, std::span<const Multiplayer::AppearanceBodyMorph> a_morphs);

    // Keeps an applied actor's morphs its own after its 3D is rebuilt, where other
    // mods write theirs. Called every frame; cheap until the 3D changes.
    void Maintain(RE::Actor& a_actor, float a_delta);

    void Release(RE::ActorHandle a_handle, RE::Actor* a_actor);
}
