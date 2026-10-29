#pragma once

#include "protocol/AppearanceState.h"

#include <RE/Skyrim.h>

#include <span>
#include <vector>

namespace RaceMenuOverlays
{
    // Asks RaceMenu's SKEE for its interfaces; at SKSE's kPostPostLoad, once every
    // plugin listens.
    void Initialize();

    [[nodiscard]] std::vector<Multiplayer::AppearanceOverlay> Capture(RE::Actor& a_player);

    void Apply(RE::Actor& a_actor, std::span<const Multiplayer::AppearanceOverlay> a_overlays);
    void Release(RE::ActorHandle a_handle, RE::Actor* a_actor);
}
