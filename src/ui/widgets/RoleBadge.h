#pragma once

#include "protocol/Role.h"
#include "sync/chat/ChatRoster.h"

#include <imgui.h>

namespace UI::RoleBadge
{
    // The side of a mark beside text of the font size.
    [[nodiscard]] float GetMarkSize(float a_fontSize);
    [[nodiscard]] float GetMarkGap(float a_fontSize);
    [[nodiscard]] float GetMarkRounding(float a_size);

    void DrawMark(ImDrawList& a_list, const ImVec2& a_min, float a_size, const ChatRoster::RoleMark& a_mark,
        float a_alpha = 1.0F);

    // The mark as an item of the current font's line height, so a line of text keeps its height.
    void Mark(const ChatRoster::RoleMark& a_mark, float a_alpha = 1.0F);

    // The role's name on its color, as an item.
    void Name(const Multiplayer::Role& a_role);
}
