#pragma once

#include <algorithm>
#include <cmath>
#include <span>

namespace Audio::SpeechLevel
{
    inline constexpr float kFloorDecibels = -50.0F;
    inline constexpr float kRangeDecibels = 40.0F;
    inline constexpr float kRelease = 0.75F;
    inline constexpr float kSilent = 0.01F;

    [[nodiscard]] inline float Measure(std::span<const float> a_frame)
    {
        float sum = 0.0F;
        for (const auto sample : a_frame)
        {
            sum += sample * sample;
        }
        const auto meanSquare = a_frame.empty() ? 0.0F : sum / static_cast<float>(a_frame.size());
        const auto decibels = 10.0F * std::log10((std::max)(meanSquare, 1e-10F));
        return std::clamp((decibels - kFloorDecibels) / kRangeDecibels, 0.0F, 1.0F);
    }

    [[nodiscard]] inline float Follow(float a_level, float a_frameLevel)
    {
        return (std::max)(a_frameLevel, a_level * kRelease);
    }

    [[nodiscard]] inline float Release(float a_level)
    {
        const auto released = a_level * kRelease;
        return released < kSilent ? 0.0F : released;
    }
}
