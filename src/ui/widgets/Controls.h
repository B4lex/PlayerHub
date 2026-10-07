#pragma once

#include <imgui.h>

#include <cstdint>
#include <span>

namespace UI::Controls
{
    enum class Icon : std::uint8_t
    {
        kClose,
        kReset
    };

    bool Toggle(const char* a_id, bool& a_value);
    [[nodiscard]] float GetToggleWidth();

    bool PrimaryButton(const char* a_label, const ImVec2& a_size = ImVec2{ 0.0F, 0.0F });

    bool DangerButton(const char* a_label, const ImVec2& a_size = ImVec2{ 0.0F, 0.0F });

    bool IconButton(const char* a_id, Icon a_icon);

    // A full-width text field with a "Search" hint. It keeps Tab, which would
    // otherwise move the keyboard to the next item.
    bool SearchField(const char* a_id, std::span<char> a_buffer);

    void MutedTextWrapped(const char* a_text);
}
