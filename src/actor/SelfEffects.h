#pragma once

#include <RE/Skyrim.h>

namespace SelfEffects
{
    // Safe from any thread once the data has loaded.

    [[nodiscard]] bool IsVoiceOrPower(const RE::MagicItem* a_item);

    // An effect of a remote actor's own that may stay on it while its source is
    // isolated.
    [[nodiscard]] bool IsSelfContained(const RE::EffectSetting& a_effect, const RE::MagicItem* a_item);

    // An effect on the caster that a sync stage owns whatever the isolation.
    [[nodiscard]] bool IsPresentationOwned(const RE::EffectSetting& a_effect);

    // A lasting effect on the player that shows on its actor and that peers apply
    // from the player's active effects rather than from the casts they see.
    [[nodiscard]] bool IsPresented(const RE::EffectSetting& a_effect);
}
