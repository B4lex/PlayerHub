#include "ui/Theme.h"
#include "config/PluginConfig.h"
#include "ui/Fonts.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <optional>

namespace UI::Theme
{
    namespace
    {
        constexpr ImVec4 kSurface{ 0.065F, 0.065F, 0.07F, 1.0F };
        constexpr ImVec4 kRaisedSurface{ 0.09F, 0.09F, 0.095F, 1.0F };
        constexpr ImVec4 kTransparent{ 0.0F, 0.0F, 0.0F, 0.0F };
        constexpr ImVec4 kText{ 0.93F, 0.92F, 0.90F, 1.0F };
        // WindowBg and PopupBg, pushed at [UI] BackgroundOpacity.
        constexpr int kBackgroundColorCount = 2;

        // The game's 999999 at the 80% it draws its message box with, as it looks
        // over the dark backing.
        constexpr ImVec4 kVanillaFrame{ 0.49F, 0.49F, 0.49F, 1.0F };
        constexpr float kAccentFrameOpacity = 0.85F;

        struct Color
        {
            ImGuiCol index;
            ImVec4 value;
        };

        constexpr Color kColors[]{
            { ImGuiCol_Text, kText },
            { ImGuiCol_TextDisabled, ImVec4{ 0.56F, 0.55F, 0.53F, 1.0F } },
            { ImGuiCol_FrameBg, Tint(0.07F) },
            { ImGuiCol_TitleBg, kRaisedSurface },
            { ImGuiCol_TitleBgActive, kRaisedSurface },
            { ImGuiCol_TitleBgCollapsed, kRaisedSurface },
            { ImGuiCol_ScrollbarBg, kTransparent },
            { ImGuiCol_ScrollbarGrab, Tint(0.18F) },
            { ImGuiCol_Button, Tint(0.10F) },
            { ImGuiCol_Separator, Tint(0.12F) },
            { ImGuiCol_ResizeGrip, kTransparent },
            { ImGuiCol_Tab, Tint(0.06F) },
            { ImGuiCol_TabDimmed, Tint(0.06F) },
            { ImGuiCol_TableBorderStrong, Tint(0.16F) },
            { ImGuiCol_TableBorderLight, Tint(0.10F) },
            { ImGuiCol_TableRowBg, kTransparent },
            { ImGuiCol_TableRowBgAlt, Tint(0.03F) },
        };

        struct AccentColor
        {
            ImGuiCol index;
            float alpha;
        };

        constexpr AccentColor kAccentColors[]{
            { ImGuiCol_Border, 0.65F },
            { ImGuiCol_FrameBgHovered, 0.20F },
            { ImGuiCol_FrameBgActive, 0.32F },
            { ImGuiCol_ScrollbarGrabHovered, 0.60F },
            { ImGuiCol_ScrollbarGrabActive, 0.90F },
            { ImGuiCol_CheckMark, 1.0F },
            { ImGuiCol_CheckboxSelectedBg, 0.20F },
            { ImGuiCol_SliderGrab, 0.80F },
            { ImGuiCol_SliderGrabActive, 1.0F },
            { ImGuiCol_ButtonHovered, 0.40F },
            { ImGuiCol_ButtonActive, 0.60F },
            { ImGuiCol_Header, 0.25F },
            { ImGuiCol_HeaderHovered, 0.35F },
            { ImGuiCol_HeaderActive, 0.50F },
            { ImGuiCol_SeparatorHovered, 0.60F },
            { ImGuiCol_SeparatorActive, 1.0F },
            { ImGuiCol_ResizeGripHovered, 0.60F },
            { ImGuiCol_ResizeGripActive, 1.0F },
            { ImGuiCol_TabHovered, 0.40F },
            { ImGuiCol_TabSelected, 0.28F },
            { ImGuiCol_TabSelectedOverline, 1.0F },
            { ImGuiCol_TabDimmedSelected, 0.28F },
            { ImGuiCol_TabDimmedSelectedOverline, 1.0F },
            { ImGuiCol_TextLink, 1.0F },
            { ImGuiCol_TextSelectedBg, 0.40F },
            { ImGuiCol_NavCursor, 1.0F },
        };

        struct Palette
        {
            ImVec4 accent;
            ImVec4 onAccent;
            ImVec4 frame;
            ImVec4 background;
            ImVec4 raisedBackground;
        };

        ImVec4 Mix(const ImVec4& a_from, const ImVec4& a_to, float a_amount)
        {
            return ImVec4{ a_from.x + (a_to.x - a_from.x) * a_amount, a_from.y + (a_to.y - a_from.y) * a_amount,
                a_from.z + (a_to.z - a_from.z) * a_amount, a_from.w + (a_to.w - a_from.w) * a_amount };
        }

        float GetLuminance(const ImVec4& a_color)
        {
            const auto linear = [](float a_channel) {
                return a_channel <= 0.04045F ? a_channel / 12.92F : std::pow((a_channel + 0.055F) / 1.055F, 2.4F);
            };
            return 0.2126F * linear(a_color.x) + 0.7152F * linear(a_color.y) + 0.0722F * linear(a_color.z);
        }

        float GetContrast(const ImVec4& a_first, const ImVec4& a_second)
        {
            const auto [darker, lighter] = std::minmax({ GetLuminance(a_first), GetLuminance(a_second) });
            return (lighter + 0.05F) / (darker + 0.05F);
        }

        const Palette& GetPalette()
        {
            static Palette palette;
            static std::optional<std::uint64_t> revision;
            const auto current = Config::GetRevision();
            if (revision != current)
            {
                revision = current;
                const auto config = Config::Get();
                palette.accent = FromRgb(config->ui.accentColor);
                palette.onAccent
                    = GetContrast(palette.accent, kOnState) >= GetContrast(palette.accent, kText) ? kOnState : kText;
                palette.frame = config->ui.frameColor == Config::UISettings::kAccentFrame
                    ? Mix(kSurface, palette.accent, kAccentFrameOpacity)
                    : kVanillaFrame;
                const auto opacity = static_cast<float>(config->ui.backgroundOpacity) * 0.01F;
                palette.background = ImVec4{ kSurface.x, kSurface.y, kSurface.z, opacity };
                palette.raisedBackground = ImVec4{ kRaisedSurface.x, kRaisedSurface.y, kRaisedSurface.z, opacity };
            }
            return palette;
        }

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

        float ToPixels(float a_fonts, float a_fontSize)
        {
            return (std::max)(std::floor(a_fonts * a_fontSize), 1.0F);
        }
    }

    ImVec4 FromRgb(std::uint32_t a_rgb)
    {
        constexpr float kChannel = 1.0F / 255.0F;
        return ImVec4{ static_cast<float>((a_rgb >> 16) & 0xFF) * kChannel,
            static_cast<float>((a_rgb >> 8) & 0xFF) * kChannel, static_cast<float>(a_rgb & 0xFF) * kChannel, 1.0F };
    }

    ImVec4 GetAccent()
    {
        return GetPalette().accent;
    }

    ImVec4 Accent(float a_alpha)
    {
        const auto& accent = GetPalette().accent;
        return ImVec4{ accent.x, accent.y, accent.z, a_alpha };
    }

    ImVec4 GetOnAccent()
    {
        return GetPalette().onAccent;
    }

    ImVec4 GetFrameColor()
    {
        return GetPalette().frame;
    }

    float GetBackgroundOpacity()
    {
        return GetPalette().background.w;
    }

    Scope::Scope()
    {
        ImGui::PushFont(Fonts::GetText(), 0.0F);
        const auto fontSize = ImGui::GetFontSize();
        for (const auto& color : kColors)
        {
            ImGui::PushStyleColor(color.index, color.value);
        }
        const auto& palette = GetPalette();
        ImGui::PushStyleColor(ImGuiCol_WindowBg, palette.background);
        ImGui::PushStyleColor(ImGuiCol_PopupBg, palette.raisedBackground);
        for (const auto& color : kAccentColors)
        {
            ImGui::PushStyleColor(color.index, Accent(color.alpha));
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
        ImGui::PopStyleColor(static_cast<int>(std::size(kColors) + kBackgroundColorCount + std::size(kAccentColors)));
        ImGui::PopFont();
    }
}
