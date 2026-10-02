#pragma once

#include "protocol/ChatMessage.h"

#include <imgui.h>

// Colors the chat adds to the theme it is drawn in. A channel has one color
// wherever it shows: a sender's name, its filter, and the send target.
namespace UI::ChatStyle
{
    inline constexpr ImVec4 kGlobalColor{ 0.95F, 0.80F, 0.45F, 1.0F };
    inline constexpr ImVec4 kLocalColor{ 0.60F, 0.95F, 0.35F, 1.0F };
    inline constexpr ImVec4 kWhisperColor{ 0.78F, 0.56F, 1.0F, 1.0F };
    inline constexpr ImVec4 kNoticeColor{ 0.95F, 0.55F, 0.40F, 1.0F };

    // Of a channel's color, behind the text of a chip in it.
    inline constexpr float kChipFillAlpha = 0.18F;
    inline constexpr float kHoveredChipFillAlpha = 0.30F;

    [[nodiscard]] constexpr const ImVec4& GetChannelColor(Multiplayer::ChatChannel a_channel)
    {
        switch (a_channel)
        {
            case Multiplayer::ChatChannel::kLocal:
                return kLocalColor;
            case Multiplayer::ChatChannel::kWhisper:
                return kWhisperColor;
            case Multiplayer::ChatChannel::kGlobal:
                break;
        }
        return kGlobalColor;
    }

    [[nodiscard]] constexpr ImVec4 WithAlpha(ImVec4 a_color, float a_alpha)
    {
        a_color.w *= a_alpha;
        return a_color;
    }
}
