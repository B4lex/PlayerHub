#pragma once

#include <RE/Skyrim.h>

namespace EffectPresentation
{
    // Safe from any thread.
    [[nodiscard]] bool IsPresentationSpell(const RE::MagicItem* a_item);

    // Game thread only. a_effect must satisfy SelfEffects::IsPresented.
    [[nodiscard]] bool Apply(RE::Actor& a_actor, RE::EffectSetting& a_effect, float a_magnitude);
    void Remove(RE::Actor& a_actor, RE::EffectSetting& a_effect);
    [[nodiscard]] bool IsApplied(RE::Actor& a_actor, const RE::EffectSetting& a_effect);
}
