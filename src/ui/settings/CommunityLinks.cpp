#include "ui/settings/CommunityLinks.h"
#include "text/Text.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"

#include <imgui.h>

#include <windows.h>

#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <span>
#include <thread>

namespace UI::CommunityLinks
{
    namespace
    {
        struct Layer
        {
            const char* glyph;
            ImU32 color;
        };

        struct Link
        {
            const char* id;
            const char* name;
            const wchar_t* url;
            Text::ID text;
            // Glyphs of the logo font that share an origin, drawn in order.
            std::span<const Layer> layers;
        };

        // U+E002 of the logo font.
        constexpr std::array kDiscordLayers{ Layer{ "\xEE\x80\x82", IM_COL32(0x58, 0x65, 0xF2, 0xFF) } };
        // U+E00E (the cup) and U+E00F (its heart) of the logo font.
        constexpr std::array kKofiLayers{ Layer{ "\xEE\x80\x8E", IM_COL32(0xFF, 0xFF, 0xFF, 0xFF) },
            Layer{ "\xEE\x80\x8F", IM_COL32(0xFF, 0x64, 0x33, 0xFF) } };

        constexpr std::array kLinks{
            Link{ "##Discord", "Discord invite", L"" PLAYERHUB_DISCORD_INVITE, Text::ID::kSettingsDiscord,
                kDiscordLayers },
            Link{ "##Kofi", "Ko-fi page", L"" PLAYERHUB_KOFI_URL, Text::ID::kSettingsKofi, kKofiLayers },
        };

        constexpr float kLogoFonts = 1.25F;
        constexpr ImVec2 kPaddingFonts{ 0.6F, 0.45F };
        constexpr float kGapFonts = 0.5F;
        constexpr float kBorderFonts = 0.0625F;
        constexpr float kIdleBorderAlpha = 0.25F;
        constexpr float kHoveredBorderAlpha = 0.6F;
        constexpr float kIdleAlpha = 0.68F;

        bool IsLinkEnabled(const Link& a_link)
        {
            return a_link.url[0] != L'\0';
        }

        void Open(const Link& a_link)
        {
            // ShellExecuteW waits for the browser to take the URL, so it stays off the
            // render thread. It may hand the URL to shell extensions, which need COM.
            std::thread([url = a_link.url, name = a_link.name] {
                const auto com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
                const auto result
                    = reinterpret_cast<INT_PTR>(::ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL));
                if (result <= 32)
                {
                    logs::warn("UI overlay: could not open the {} (ShellExecute {})", name, result);
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
            ImFont* textFont;
            float textFontSize;
            ImVec2 textSize;
            ImVec2 padding;
            ImFont* logo;
            float logoSize;
            float logoWidth;
            float gap;
            ImVec2 size;
        };

        Layout Measure(const Link& a_link)
        {
            const auto fontSize = ImGui::GetFontSize();
            auto* heading = Fonts::GetHeading();
            Layout layout{ .text = Text::GetCString(a_link.text),
                .textFont = heading ? heading : ImGui::GetFont(),
                .textFontSize = fontSize,
                .padding = ImVec2{ std::floor(kPaddingFonts.x * fontSize), std::floor(kPaddingFonts.y * fontSize) },
                .logo = Fonts::GetLogo(),
                // The logo font's em is the glyph's height; Dear ImGui rounds font sizes to whole pixels.
                .logoSize = std::floor(kLogoFonts * fontSize) };
            layout.textSize = layout.textFont->CalcTextSizeA(layout.textFontSize, FLT_MAX, 0.0F, layout.text);
            if (layout.logo)
            {
                layout.logoWidth = std::ceil(
                    layout.logo->CalcTextSizeA(layout.logoSize, FLT_MAX, 0.0F, a_link.layers.front().glyph).x);
                layout.gap = std::floor(kGapFonts * fontSize);
            }
            layout.size = ImVec2{ layout.padding.x * 2.0F + layout.logoWidth + layout.gap + layout.textSize.x,
                layout.logoSize + layout.padding.y * 2.0F };
            return layout;
        }

        void DrawLink(const Link& a_link, const Layout& a_layout)
        {
            const auto fontSize = ImGui::GetFontSize();
            const auto min = ImGui::GetCursorScreenPos();
            const bool pressed = ImGui::InvisibleButton(a_link.id, a_layout.size);
            const bool hovered = ImGui::IsItemHovered();
            if (hovered)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }
            if (pressed)
            {
                Open(a_link);
            }

            auto& list = *ImGui::GetWindowDrawList();
            const ImVec2 max{ min.x + a_layout.size.x, min.y + a_layout.size.y };
            const auto border = (std::max)(std::floor(kBorderFonts * fontSize), 1.0F);
            // Inset by half the line, so its outer edge stays on the button's bounds.
            const auto half = border * 0.5F;
            list.AddRect(ImVec2{ min.x + half, min.y + half }, ImVec2{ max.x - half, max.y - half },
                ImGui::GetColorU32(hovered ? Theme::Tint(kHoveredBorderAlpha) : Theme::Tint(kIdleBorderAlpha)),
                ImGui::GetStyle().FrameRounding, ImDrawFlags_None, border);
            const auto left = min.x + a_layout.padding.x;
            if (a_layout.logo)
            {
                for (const auto& layer : a_link.layers)
                {
                    list.AddText(a_layout.logo, a_layout.logoSize, ImVec2{ left, min.y + a_layout.padding.y },
                        layer.color, layer.glyph);
                }
            }
            list.AddText(a_layout.textFont, a_layout.textFontSize,
                ImVec2{ left + a_layout.logoWidth + a_layout.gap,
                    min.y + std::floor((a_layout.size.y - a_layout.textSize.y) * 0.5F) },
                ImGui::GetColorU32(ImGuiCol_Text, hovered ? 1.0F : kIdleAlpha), a_layout.text);
        }
    }

    bool IsEnabled()
    {
        return std::ranges::any_of(kLinks, IsLinkEnabled);
    }

    ImVec2 GetSize()
    {
        ImVec2 size{};
        for (const auto& link : kLinks)
        {
            if (!IsLinkEnabled(link))
            {
                continue;
            }
            const auto linkSize = Measure(link).size;
            size.x += (size.x > 0.0F ? ImGui::GetStyle().ItemSpacing.x : 0.0F) + linkSize.x;
            size.y = (std::max)(size.y, linkSize.y);
        }
        return size;
    }

    void Draw()
    {
        bool first = true;
        for (const auto& link : kLinks)
        {
            if (!IsLinkEnabled(link))
            {
                continue;
            }
            if (!first)
            {
                ImGui::SameLine();
            }
            first = false;
            DrawLink(link, Measure(link));
        }
    }
}
