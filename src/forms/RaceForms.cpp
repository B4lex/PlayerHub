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

    RE::TESRace::FaceRelatedData* GetFaceRelatedData(RE::TESRace* a_race, bool a_female)
    {
        return a_race ? a_race->faceRelatedData[a_female ? RE::SEXES::kFemale : RE::SEXES::kMale] : nullptr;
    }

    RE::BGSHeadPart* FindDefaultHeadPart(const RE::TESRace::FaceRelatedData* a_faceData, std::uint32_t a_type)
    {
        if (!a_faceData || !a_faceData->headParts)
        {
            return nullptr;
        }
        for (auto* headPart : *a_faceData->headParts)
        {
            if (headPart && headPart->type.underlying() == a_type)
            {
                return headPart;
            }
        }
        return nullptr;
    }
}
