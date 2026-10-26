#pragma once

#include <RE/Skyrim.h>

namespace Forms
{
    [[nodiscard]] bool IsPlayableRace(const RE::TESRace* a_race);

    // The race itself when playable, else its morph race (NAM8) when that is playable,
    // as for the vanilla *RaceVampire races; null otherwise.
    [[nodiscard]] RE::TESRace* GetPlayableRace(RE::TESRace* a_race);
}
