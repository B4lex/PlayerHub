#pragma once

#include <imgui.h>

namespace UI::Theme
{
    // FF9000. Marks what the player can act on or has selected.
    inline constexpr ImVec4 kAccent{ 1.0F, 144.0F / 255.0F, 0.0F, 1.0F };
    // Text and marks on an accent or state fill.
    inline constexpr ImVec4 kOnAccent{ 0.09F, 0.06F, 0.02F, 1.0F };

    // What a state means, apart from the accent: working, needing attention,
    // and failed.
    inline constexpr ImVec4 kPositive{ 0.45F, 0.85F, 0.45F, 1.0F };
    inline constexpr ImVec4 kWarning{ 0.95F, 0.70F, 0.35F, 1.0F };
    inline constexpr ImVec4 kNegative{ 0.95F, 0.40F, 0.35F, 1.0F };

    constexpr ImVec4 Accent(float a_alpha)
    {
        return ImVec4{ kAccent.x, kAccent.y, kAccent.z, a_alpha };
    }

    // White over the surface below, so a mark stays visible on any of them.
    constexpr ImVec4 Tint(float a_alpha)
    {
        return ImVec4{ 1.0F, 1.0F, 1.0F, a_alpha };
    }

    // The look of the plugin's interactive windows: the themed text face,
    // dark neutral surfaces, rounded corners, an accent border around windows
    // and popups, and accent highlights on widgets. Applies to every window
    // and popup begun while it lives, so a window wraps its Begin and End in
    // one; the overlay's base style stays as it is for the others. Overlay
    // thread, between ImGui::NewFrame and ImGui::Render.
    class Scope
    {
    public:
        Scope();
        ~Scope();

        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
    };
}
