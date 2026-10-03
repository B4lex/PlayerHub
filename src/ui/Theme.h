#pragma once

#include <imgui.h>

#include <cstdint>

namespace UI::Theme
{
    // What a state means, apart from the accent: working, needing attention,
    // and failed.
    inline constexpr ImVec4 kPositive{ 0.45F, 0.85F, 0.45F, 1.0F };
    inline constexpr ImVec4 kWarning{ 0.95F, 0.70F, 0.35F, 1.0F };
    inline constexpr ImVec4 kNegative{ 0.95F, 0.40F, 0.35F, 1.0F };
    // Text and marks on a state fill.
    inline constexpr ImVec4 kOnState{ 0.09F, 0.06F, 0.02F, 1.0F };

    // [UI] AccentColor, opaque. Marks what the player can act on or has
    // selected. The accent functions follow a change on their next call.
    // Overlay thread.
    [[nodiscard]] ImVec4 GetAccent();
    [[nodiscard]] ImVec4 Accent(float a_alpha);
    // Text and marks on an accent fill: dark on a light accent, light on a
    // dark one, whichever contrasts more.
    [[nodiscard]] ImVec4 GetOnAccent();

    // An opaque color from 0xRRGGBB, as [UI] AccentColor stores it.
    [[nodiscard]] ImVec4 FromRgb(std::uint32_t a_rgb);

    // White over the surface below, so a mark stays visible on any of them.
    constexpr ImVec4 Tint(float a_alpha)
    {
        return ImVec4{ 1.0F, 1.0F, 1.0F, a_alpha };
    }

    // The menu frame around a window, as [UI] BorderColor has it: the grey of
    // the game's menus or the accent. Opaque. Overlay thread.
    [[nodiscard]] ImVec4 GetFrameColor();

    // The look of the plugin's interactive windows: the themed text face,
    // dark neutral surfaces, rounded corners, an accent border around popups,
    // and accent highlights on widgets. Applies to every window and popup
    // begun while it lives, so a window wraps its Begin and End in one; the
    // overlay's base style stays as it is for the others. Overlay thread,
    // between ImGui::NewFrame and ImGui::Render.
    class Scope
    {
    public:
        Scope();
        ~Scope();

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };
}
