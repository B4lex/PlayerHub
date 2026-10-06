#pragma once

#include "protocol/ChatMessage.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace UI::ChatStyle
{
    inline constexpr ImVec4 kGlobalColor{ 0.95F, 0.80F, 0.45F, 1.0F };
    inline constexpr ImVec4 kLocalColor{ 0.60F, 0.95F, 0.35F, 1.0F };
    inline constexpr ImVec4 kWhisperColor{ 0.78F, 0.56F, 1.0F, 1.0F };
    inline constexpr ImVec4 kNoticeColor{ 0.95F, 0.55F, 0.40F, 1.0F };

    inline constexpr float kChipFillAlpha = 0.18F;
    inline constexpr float kHoveredChipFillAlpha = 0.30F;
    inline constexpr float kHoveredOffChipFillAlpha = 0.08F;
    inline constexpr float kChipOutlineAlpha = 0.55F;
    inline constexpr ImVec2 kChipPaddingFonts{ 0.5F, 0.25F };
    inline constexpr float kChipOutlineFonts = 0.0625F;

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

    [[nodiscard]] inline ImVec2 GetChipPadding()
    {
        const auto fontSize = ImGui::GetFontSize();
        return ImVec2{ std::floor(kChipPaddingFonts.x * fontSize), std::floor(kChipPaddingFonts.y * fontSize) };
    }

    inline void DrawChipFrame(const ImVec4& a_color, float a_fillAlpha)
    {
        const auto& style = ImGui::GetStyle();
        const auto min = ImGui::GetItemRectMin();
        const auto max = ImGui::GetItemRectMax();
        const auto outline = (std::max)(std::floor(kChipOutlineFonts * ImGui::GetFontSize()), 1.0F);
        auto* drawList = ImGui::GetWindowDrawList();
        drawList->AddRectFilled(min, max, ImGui::GetColorU32(WithAlpha(a_color, a_fillAlpha)), style.FrameRounding);
        drawList->AddRect(min, max, ImGui::GetColorU32(WithAlpha(a_color, kChipOutlineAlpha)), style.FrameRounding,
            ImDrawFlags_None, outline);
    }

    inline void DrawRule(float a_gap)
    {
        const auto& style = ImGui::GetStyle();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - style.ItemSpacing.y + a_gap);
        ImGui::Dummy(ImVec2{ ImGui::GetContentRegionAvail().x, style.WindowBorderSize });
        ImGui::GetWindowDrawList()->AddRectFilled(
            ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), ImGui::GetColorU32(ImGuiCol_Separator));
    }
}
