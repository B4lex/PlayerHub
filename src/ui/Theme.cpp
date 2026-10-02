#include "ui/Theme.h"
#include "ui/Fonts.h"

#include <algorithm>
#include <cmath>
#include <iterator>

namespace UI::Theme
{
    namespace
    {
        constexpr ImVec4 kSurface{ 0.065F, 0.065F, 0.07F, 1.0F };
        constexpr ImVec4 kRaisedSurface{ 0.09F, 0.09F, 0.095F, 1.0F };
        constexpr ImVec4 kTransparent{ 0.0F, 0.0F, 0.0F, 0.0F };

        struct Color
        {
            ImGuiCol index;
            ImVec4 value;
        };

        constexpr Color kColors[]{
            { ImGuiCol_Text, ImVec4{ 0.93F, 0.92F, 0.90F, 1.0F } },
            { ImGuiCol_TextDisabled, ImVec4{ 0.56F, 0.55F, 0.53F, 1.0F } },
            { ImGuiCol_WindowBg, kSurface },
            { ImGuiCol_PopupBg, kRaisedSurface },
            { ImGuiCol_Border, Accent(0.65F) },
            { ImGuiCol_FrameBg, Tint(0.07F) },
            { ImGuiCol_FrameBgHovered, Accent(0.20F) },
            { ImGuiCol_FrameBgActive, Accent(0.32F) },
            { ImGuiCol_TitleBg, kRaisedSurface },
            { ImGuiCol_TitleBgActive, kRaisedSurface },
            { ImGuiCol_TitleBgCollapsed, kRaisedSurface },
            { ImGuiCol_ScrollbarBg, kTransparent },
            { ImGuiCol_ScrollbarGrab, Tint(0.18F) },
            { ImGuiCol_ScrollbarGrabHovered, Accent(0.60F) },
            { ImGuiCol_ScrollbarGrabActive, Accent(0.90F) },
            { ImGuiCol_CheckMark, kAccent },
            { ImGuiCol_CheckboxSelectedBg, Accent(0.20F) },
            { ImGuiCol_SliderGrab, Accent(0.80F) },
            { ImGuiCol_SliderGrabActive, kAccent },
            { ImGuiCol_Button, Tint(0.10F) },
            { ImGuiCol_ButtonHovered, Accent(0.40F) },
            { ImGuiCol_ButtonActive, Accent(0.60F) },
            { ImGuiCol_Header, Accent(0.25F) },
            { ImGuiCol_HeaderHovered, Accent(0.35F) },
            { ImGuiCol_HeaderActive, Accent(0.50F) },
            { ImGuiCol_Separator, Tint(0.12F) },
            { ImGuiCol_SeparatorHovered, Accent(0.60F) },
            { ImGuiCol_SeparatorActive, kAccent },
            { ImGuiCol_ResizeGrip, kTransparent },
            { ImGuiCol_ResizeGripHovered, Accent(0.60F) },
            { ImGuiCol_ResizeGripActive, kAccent },
            { ImGuiCol_Tab, Tint(0.06F) },
            { ImGuiCol_TabHovered, Accent(0.40F) },
            { ImGuiCol_TabSelected, Accent(0.28F) },
            { ImGuiCol_TabSelectedOverline, kAccent },
            { ImGuiCol_TabDimmed, Tint(0.06F) },
            { ImGuiCol_TabDimmedSelected, Accent(0.28F) },
            { ImGuiCol_TabDimmedSelectedOverline, kAccent },
            { ImGuiCol_TableBorderStrong, Tint(0.16F) },
            { ImGuiCol_TableBorderLight, Tint(0.10F) },
            { ImGuiCol_TableRowBg, kTransparent },
            { ImGuiCol_TableRowBgAlt, Tint(0.03F) },
            { ImGuiCol_TextLink, kAccent },
            { ImGuiCol_TextSelectedBg, Accent(0.40F) },
            { ImGuiCol_NavCursor, kAccent },
        };

        // Sizes are in font sizes, so they follow the overlay scale.
        struct Size
        {
            ImGuiStyleVar index;
            float value;
        };

        struct Extent
        {
            ImGuiStyleVar index;
            ImVec2 value;
        };

        // One pixel at the overlay's base font size.
        constexpr float kLine = 1.0F / 16.0F;

        constexpr Size kSizes[]{
            { ImGuiStyleVar_WindowRounding, 0.625F },
            { ImGuiStyleVar_WindowBorderSize, kLine },
            { ImGuiStyleVar_PopupRounding, 0.5F },
            { ImGuiStyleVar_PopupBorderSize, kLine },
            { ImGuiStyleVar_ChildRounding, 0.5F },
            { ImGuiStyleVar_ChildBorderSize, kLine },
            { ImGuiStyleVar_FrameRounding, 0.375F },
            { ImGuiStyleVar_GrabRounding, 0.375F },
            { ImGuiStyleVar_TabRounding, 0.375F },
            { ImGuiStyleVar_TabBarBorderSize, kLine },
            { ImGuiStyleVar_TabBarOverlineSize, 2.0F * kLine },
            { ImGuiStyleVar_ScrollbarSize, 0.625F },
            { ImGuiStyleVar_ScrollbarRounding, 0.5F },
        };

        constexpr Extent kExtents[]{
            { ImGuiStyleVar_WindowPadding, ImVec2{ 0.75F, 0.625F } },
            { ImGuiStyleVar_FramePadding, ImVec2{ 0.625F, 0.375F } },
            { ImGuiStyleVar_ItemSpacing, ImVec2{ 0.5F, 0.5F } },
            { ImGuiStyleVar_CellPadding, ImVec2{ 0.375F, 0.25F } },
        };

        // Whole pixels keep lines and corners sharp, and a line never vanishes
        // at a small scale.
        float ToPixels(float a_fonts, float a_fontSize)
        {
            return (std::max)(std::floor(a_fonts * a_fontSize), 1.0F);
        }
    }

    Scope::Scope()
    {
        ImGui::PushFont(Fonts::GetText(), 0.0F);
        const auto fontSize = ImGui::GetFontSize();
        for (const auto& color : kColors)
        {
            ImGui::PushStyleColor(color.index, color.value);
        }
        for (const auto& size : kSizes)
        {
            ImGui::PushStyleVar(size.index, ToPixels(size.value, fontSize));
        }
        for (const auto& extent : kExtents)
        {
            ImGui::PushStyleVar(
                extent.index, ImVec2{ ToPixels(extent.value.x, fontSize), ToPixels(extent.value.y, fontSize) });
        }
    }

    Scope::~Scope()
    {
        ImGui::PopStyleVar(static_cast<int>(std::size(kSizes) + std::size(kExtents)));
        ImGui::PopStyleColor(static_cast<int>(std::size(kColors)));
        ImGui::PopFont();
    }
}
