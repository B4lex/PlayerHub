#pragma once

#include <imgui.h>

#include <cstdint>

namespace UI::Theme
{
    inline constexpr ImVec4 kPositive{ 0.45F, 0.85F, 0.45F, 1.0F };
    inline constexpr ImVec4 kWarning{ 0.95F, 0.70F, 0.35F, 1.0F };
    inline constexpr ImVec4 kNegative{ 0.95F, 0.40F, 0.35F, 1.0F };
    inline constexpr ImVec4 kOnState{ 0.09F, 0.06F, 0.02F, 1.0F };

    [[nodiscard]] ImVec4 GetAccent();
    [[nodiscard]] ImVec4 Accent(float a_alpha);
    [[nodiscard]] ImVec4 GetOnAccent();

    [[nodiscard]] ImVec4 FromRgb(std::uint32_t a_rgb);

    constexpr ImVec4 Tint(float a_alpha)
    {
        return ImVec4{ 1.0F, 1.0F, 1.0F, a_alpha };
    }

    [[nodiscard]] ImVec4 GetFrameColor();

    class Scope
    {
    public:
        Scope();
        ~Scope();

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };
}
