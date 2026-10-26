#include "forms/RaceForms.h"

namespace Forms
{
    bool IsPlayableRace(const RE::TESRace* a_race)
    {
        return a_race && a_race->data.flags.all(RE::RACE_DATA::Flag::kPlayable);
    }

    RE::TESRace* GetPlayableRace(RE::TESRace* a_race)
    {
        if (IsPlayableRace(a_race))
        {
            return a_race;
        }
        auto* morphRace = a_race ? a_race->morphRace : nullptr;
        return IsPlayableRace(morphRace) ? morphRace : nullptr;
    }
}
