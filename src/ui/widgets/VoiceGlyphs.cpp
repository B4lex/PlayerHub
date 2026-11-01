#include "ui/widgets/VoiceGlyphs.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace UI::VoiceGlyphs
{
    namespace
    {
        // Each bar's share of the level, so they rise unevenly, as an equalizer's do.
        constexpr std::array<float, 3> kBarWeights{ 0.6F, 1.0F, 0.75F };
        constexpr float kRestingHeight = 0.25F;
    }

    void DrawSpeaking(ImDrawList& a_drawList, ImVec2 a_min, float a_size, ImU32 a_color, float a_level)
    {
        const auto gap = std::floor(a_size / 7.0F);
        const auto width = std::floor((a_size - gap * 2.0F) / 3.0F);
        const auto bottom = a_min.y + a_size;
        const auto level = std::clamp(a_level, 0.0F, 1.0F);
        for (std::size_t bar = 0; bar < kBarWeights.size(); ++bar)
        {
            const auto height = std::round(a_size * std::lerp(kRestingHeight, 1.0F, level * kBarWeights[bar]));
            const auto left = a_min.x + static_cast<float>(bar) * (width + gap);
            a_drawList.AddRectFilled(ImVec2{ left, bottom - height }, ImVec2{ left + width, bottom }, a_color,
                width * 0.5F);
        }
    }

    void DrawMicrophone(ImDrawList& a_drawList, ImVec2 a_min, float a_size, ImU32 a_color, bool a_off)
    {
        const auto line = (std::max)(1.0F, std::round(a_size / 10.0F));
        const auto centerX = a_min.x + a_size * 0.5F;
        const auto capsuleWidth = a_size * 0.4F;
        const ImVec2 capsuleMin{ centerX - capsuleWidth * 0.5F, a_min.y };
        const ImVec2 capsuleMax{ centerX + capsuleWidth * 0.5F, a_min.y + a_size * 0.6F };
        a_drawList.AddRectFilled(capsuleMin, capsuleMax, a_color, capsuleWidth * 0.5F);

        // The holder: an arc under the capsule, a stem, and a foot.
        const auto arcCenterY = a_min.y + a_size * 0.4F;
        const auto arcRadius = a_size * 0.33F;
        a_drawList.PathArcTo(ImVec2{ centerX, arcCenterY }, arcRadius, 0.0F, std::numbers::pi_v<float>);
        a_drawList.PathStroke(a_color, ImDrawFlags_None, line);
        const auto stemTop = arcCenterY + arcRadius;
        const auto bottom = a_min.y + a_size;
        a_drawList.AddLine(ImVec2{ centerX, stemTop }, ImVec2{ centerX, bottom }, a_color, line);
        a_drawList.AddLine(ImVec2{ centerX - a_size * 0.2F, bottom }, ImVec2{ centerX + a_size * 0.2F, bottom }, a_color,
            line);

        if (a_off)
        {
            a_drawList.AddLine(a_min, ImVec2{ a_min.x + a_size, a_min.y + a_size }, a_color, line * 1.5F);
        }
    }
}
