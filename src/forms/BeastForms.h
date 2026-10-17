#pragma once

#include "protocol/Shapeshift.h"

#include <RE/Skyrim.h>

#include <array>

namespace Forms
{
    void ResolveBeastForms();

    // Null for kNone, or while the defining master is not loaded.
    [[nodiscard]] RE::TESRace* GetBeastRace(Multiplayer::Shapeshift a_form);

    // kNone for any race but the three vanilla beast races.
    [[nodiscard]] Multiplayer::Shapeshift GetShapeshift(const RE::TESRace* a_race);

    enum class VampireLordPower
    {
        kMistForm,
        kBats
    };

    // The power's vanilla magic effect and the look it gives the vampire lord.
    struct VampireLordPowerForms
    {
        RE::EffectSetting* effect{ nullptr };
        std::array<RE::TESEffectShader*, 2> shaders{};
        RE::BGSArtObject* art{ nullptr };
        float alpha{ 1.0F };
    };

    [[nodiscard]] const VampireLordPowerForms& GetVampireLordPower(VampireLordPower a_power);
}
