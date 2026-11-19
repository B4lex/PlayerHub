#include "ui/widgets/TextShadow.h"

#include <array>
#include <cmath>

namespace UI::TextShadow
{
    namespace
    {
        constexpr std::array<ImVec2, 4> kOffsetsFonts{
            { { 0.02F, 0.07F }, { 0.12F, 0.07F },
            { 0.07F, 0.02F }, { 0.07F, 0.12F } }
        };
        constexpr float kReachFonts = 0.135F;
        constexpr float kAlpha = 0.25F;
    }

    float GetReach()
    {
        return std::ceil(kReachFonts * ImGui::GetFontSize());
    }

    std::span<const ImVec2> GetOffsetsFonts()
    {
        return kOffsetsFonts;
    }

    void Darken(int a_firstVertex, const ImVec2& a_offset)
    {
        auto& vertices = ImGui::GetWindowDrawList()->VtxBuffer;
        for (auto index = a_firstVertex; index < vertices.Size; ++index)
        {
            auto& vertex = vertices[index];
            vertex.pos.x += a_offset.x;
            vertex.pos.y += a_offset.y;
            const auto alpha = static_cast<float>((vertex.col >> IM_COL32_A_SHIFT) & 0xFFU);
            vertex.col = IM_COL32(0, 0, 0, static_cast<int>(alpha * kAlpha + 0.5F));
        }
    }
}
