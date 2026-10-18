#include "ui/settings/DiscordLink.h"
#include "text/Text.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"

#include <imgui.h>

#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <thread>

namespace UI::DiscordLink
{
    namespace
    {
        constexpr const wchar_t* kInviteUrl = L"" PLAYERHUB_DISCORD_INVITE;
        // U+E002 of the logo font.
        constexpr const char* kDiscordGlyph = "\xEE\x80\x82";
        constexpr ImU32 kDiscordColor = IM_COL32(0x58, 0x65, 0xF2, 0xFF);
        constexpr float kLogoFonts = 1.25F;
        constexpr ImVec2 kPaddingFonts{ 0.4F, 0.45F };
        constexpr float kGapFonts = 0.5F;
        constexpr float kBorderFonts = 0.0625F;
        constexpr float kIdleBorderAlpha = 0.25F;
        constexpr float kHoveredBorderAlpha = 0.6F;
        constexpr float kIdleAlpha = 0.68F;

        void OpenInvite()
        {
            // ShellExecuteW waits for the browser to take the URL, so it stays off the
            // render thread. It may hand the URL to shell extensions, which need COM.
            std::thread([] {
                const auto com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
                const auto result = reinterpret_cast<INT_PTR>(
                    ::ShellExecuteW(nullptr, L"open", kInviteUrl, nullptr, nullptr, SW_SHOWNORMAL));
                if (result <= 32)
                {
                    logs::warn("UI overlay: could not open the Discord invite (ShellExecute {})", result);
                }
                if (SUCCEEDED(com))
                {
                    ::CoUninitialize();
                }
            }).detach();
        }

        struct Layout
        {
            const char* text;
            ImVec2 textSize;
            ImVec2 padding;
            ImFont* logo;
            float logoSize;
            float logoWidth;
            float gap;
            ImVec2 size;
        };

        Layout Measure()
        {
            const auto fontSize = ImGui::GetFontSize();
            Layout layout{ .text = Text::GetCString(Text::Id::kSettingsDiscord),
                .padding = ImVec2{ std::floor(kPaddingFonts.x * fontSize), std::floor(kPaddingFonts.y * fontSize) },
                .logo = Fonts::GetLogo(),
                // The logo font's em is the glyph's height; Dear ImGui rounds font sizes to whole pixels.
                .logoSize = std::floor(kLogoFonts * fontSize) };
            layout.textSize = ImGui::CalcTextSize(layout.text);
            if (layout.logo)
            {
                layout.logoWidth
                    = std::ceil(layout.logo->CalcTextSizeA(layout.logoSize, FLT_MAX, 0.0F, kDiscordGlyph).x);
                layout.gap = std::floor(kGapFonts * fontSize);
            }
            layout.size = ImVec2{ layout.padding.x * 2.0F + layout.logoWidth + layout.gap + layout.textSize.x,
                layout.logoSize + layout.padding.y * 2.0F };
            return layout;
        }
    }

    bool IsEnabled()
    {
        return kInviteUrl[0] != L'\0';
    }

    ImVec2 GetSize()
    {
        return Measure().size;
    }

    void Draw()
    {
        const auto fontSize = ImGui::GetFontSize();
        const auto layout = Measure();
        const auto min = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton("##Discord", layout.size);
        const bool hovered = ImGui::IsItemHovered();
        if (hovered)
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
        if (pressed)
        {
            OpenInvite();
        }

        auto& list = *ImGui::GetWindowDrawList();
        const ImVec2 max{ min.x + layout.size.x, min.y + layout.size.y };
        const auto border = (std::max)(std::floor(kBorderFonts * fontSize), 1.0F);
        // Inset by half the line, so its outer edge stays on the button's bounds.
        const auto half = border * 0.5F;
        list.AddRect(ImVec2{ min.x + half, min.y + half }, ImVec2{ max.x - half, max.y - half },
            ImGui::GetColorU32(hovered ? Theme::Tint(kHoveredBorderAlpha) : Theme::Tint(kIdleBorderAlpha)),
            ImGui::GetStyle().FrameRounding, ImDrawFlags_None, border);
        const auto left = min.x + layout.padding.x;
        if (layout.logo)
        {
            list.AddText(layout.logo, layout.logoSize, ImVec2{ left, min.y + layout.padding.y }, kDiscordColor,
                kDiscordGlyph);
        }
        list.AddText(ImVec2{ left + layout.logoWidth + layout.gap,
                         min.y + std::floor((layout.size.y - layout.textSize.y) * 0.5F) },
            ImGui::GetColorU32(ImGuiCol_Text, hovered ? 1.0F : kIdleAlpha), layout.text);
    }
}
