#pragma once

#include <RE/Skyrim.h>

#include <cstdint>

namespace Forms
{
    [[nodiscard]] bool IsPlayableRace(const RE::TESRace* a_race);

    // The race itself when playable, else its morph race (NAM8) when that is playable,
    // as for the vanilla *RaceVampire races; null otherwise.
    [[nodiscard]] RE::TESRace* GetPlayableRace(RE::TESRace* a_race);

    [[nodiscard]] RE::TESRace::FaceRelatedData* GetFaceRelatedData(RE::TESRace* a_race, bool a_female);

    // The first of the face data's default head parts with this BGSHeadPart::type.
    [[nodiscard]] RE::BGSHeadPart* FindDefaultHeadPart(
        const RE::TESRace::FaceRelatedData* a_faceData, std::uint32_t a_type);
}
