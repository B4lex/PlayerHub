#pragma once

#include "protocol/FormReference.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace Multiplayer
{
    struct ActiveEffect
    {
        FormReference effect;
        float magnitude{ 0.0F };

        bool operator==(const ActiveEffect&) const = default;
    };

    struct PlayerEffects
    {
        static constexpr std::size_t kMaximumEffects = 32;
        static constexpr float kMaximumMagnitude = 1.0e6F;

        std::uint64_t revision{ 0 };
        // Ordered by effect reference, without duplicates.
        std::vector<ActiveEffect> effects;
    };

    [[nodiscard]] inline bool IsValidEffects(const PlayerEffects& a_effects)
    {
        if (a_effects.revision == 0
            || a_effects.revision > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())
            || a_effects.effects.size() > PlayerEffects::kMaximumEffects)
        {
            return false;
        }
        for (std::size_t i = 0; i < a_effects.effects.size(); ++i)
        {
            const auto& entry = a_effects.effects[i];
            if (entry.effect.IsEmpty() || !IsValidFormReference(entry.effect, false) || !std::isfinite(entry.magnitude)
                || std::fabs(entry.magnitude) > PlayerEffects::kMaximumMagnitude)
            {
                return false;
            }
            for (std::size_t j = 0; j < i; ++j)
            {
                if (a_effects.effects[j].effect == entry.effect)
                {
                    return false;
                }
            }
        }
        return true;
    }
}
