#pragma once

#include <imgui.h>

#include <cstdint>

namespace UI::Controls
{
    // Drawn as lines, so they do not depend on a font having the glyph.
    enum class Icon : std::uint8_t
    {
        kClose,
        kReset
    };

    // Small controls the plugin's interactive windows share. Each is as high as
    // a frame, takes the colors of the style it is drawn in, and returns true
    // when pressed. Overlay thread, inside a window.

    // A switch for an on or off value, which a press flips.
    bool Toggle(const char* a_id, bool& a_value);
    [[nodiscard]] float GetToggleWidth();

    // A button with the accent fill, for the action a view leads to.
    bool PrimaryButton(const char* a_label, const ImVec2& a_size = ImVec2{ 0.0F, 0.0F });

    // A button with the red fill, for an action that ends what is running.
    bool DangerButton(const char* a_label, const ImVec2& a_size = ImVec2{ 0.0F, 0.0F });

    // A square button showing only an icon.
    bool IconButton(const char* a_id, Icon a_icon);
}
