#pragma once

#include "protocol/AppearanceState.h"

#include <RE/Skyrim.h>

#include <span>
#include <vector>

namespace RaceMenuFaceSliders
{
    // The player's non-zero RaceMenu slider values for the race and sex the
    // appearance carries, sorted by name.
    [[nodiscard]] std::vector<Multiplayer::AppearanceFaceSlider> Capture(
        RE::TESNPC& a_playerBase, const RE::TESRace& a_race, bool a_female);

    // Gives a_base the slider values before an actor of it loads its 3D. SKEE drops
    // every base's values when a save loads, so this runs before each spawn.
    void Apply(RE::TESNPC& a_base, std::span<const Multiplayer::AppearanceFaceSlider> a_sliders);
}
