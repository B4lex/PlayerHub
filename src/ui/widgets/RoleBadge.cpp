#include "ui/widgets/RoleBadge.h"
#include "ui/Fonts.h"
#include "ui/chat/ChatStyle.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

namespace UI::RoleBadge
{
    namespace
    {
        constexpr float kMarkFonts = 0.9F;
        constexpr float kMarkGapFonts = 0.3F;
        // A chat chip's frame rounding over its height.
        constexpr float kMarkRounding = 0.25F;
        constexpr float kLetterScale = 0.78F;

        struct LetterBounds
        {
            char32_t code{ 0 };
            std::optional<Fonts::GlyphBounds> bounds;
        };

        // A few roles at most, so a short list read every frame by every mark.
        std::vector<LetterBounds> s_letterBounds;

        std::optional<Fonts::GlyphBounds> FindLetterBounds(char32_t a_code)
        {
            const auto found = std::ranges::find(s_letterBounds, a_code, &LetterBounds::code);
            if (found != s_letterBounds.end())
            {
                return found->bounds;
            }
            return s_letterBounds.emplace_back(a_code, Fonts::FindHeadingBounds(a_code)).bounds;
        }

        ImVec4 ToColor(std::uint32_t a_color, float a_alpha)
        {
            return ImVec4{ static_cast<float>((a_color >> 16) & 0xFF) / 255.0F,
                static_cast<float>((a_color >> 8) & 0xFF) / 255.0F, static_cast<float>(a_color & 0xFF) / 255.0F,
                a_alpha };
        }

        void DrawChip(ImDrawList& a_list, const ImVec2& a_min, const ImVec2& a_max, const ImVec4& a_color,
            float a_rounding, float a_fontSize)
        {
            const auto outline = (std::max)(std::floor(ChatStyle::kChipOutlineFonts * a_fontSize), 1.0F);
            a_list.AddRectFilled(a_min, a_max,
                ImGui::ColorConvertFloat4ToU32(ChatStyle::WithAlpha(a_color, ChatStyle::kChipFillAlpha)), a_rounding);
            a_list.AddRect(a_min, a_max,
                ImGui::ColorConvertFloat4ToU32(ChatStyle::WithAlpha(a_color, ChatStyle::kChipOutlineAlpha)),
                a_rounding, ImDrawFlags_None, outline);
        }

        ImFont* GetLetterFont()
        {
            auto* font = Fonts::GetHeading();
            return font ? font : ImGui::GetFont();
        }

        char32_t ReadCodePoint(const char* a_text)
        {
            const auto byte = [a_text](int a_index) { return static_cast<unsigned char>(a_text[a_index]); };
            const auto tail = [&byte](int a_index) { return static_cast<char32_t>(byte(a_index) & 0x3F); };
            const auto lead = byte(0);
            if (lead < 0x80)
            {
                return lead;
            }
            if (lead < 0xE0)
            {
                return (static_cast<char32_t>(lead & 0x1F) << 6) | tail(1);
            }
            if (lead < 0xF0)
            {
                return (static_cast<char32_t>(lead & 0x0F) << 12) | (tail(1) << 6) | tail(2);
            }
            return (static_cast<char32_t>(lead & 0x07) << 18) | (tail(1) << 12) | (tail(2) << 6) | tail(3);
        }
    }

    float GetMarkSize(float a_fontSize)
    {
        return std::round(kMarkFonts * a_fontSize);
    }

    float GetMarkGap(float a_fontSize)
    {
        return std::round(kMarkGapFonts * a_fontSize);
    }

    float GetMarkRounding(float a_size)
    {
        return std::round(kMarkRounding * a_size);
    }

    void DrawMark(ImDrawList& a_list, const ImVec2& a_min, float a_size, const ChatRoster::RoleMark& a_mark, float a_alpha)
    {
        const auto color = ToColor(a_mark.color, a_alpha);
        DrawChip(a_list, a_min, ImVec2{ a_min.x + a_size, a_min.y + a_size }, color, GetMarkRounding(a_size),
            a_size / kMarkFonts);

        auto* font = GetLetterFont();
        const auto letterSize = std::round(kLetterScale * a_size);
        // Zero-terminated: a mark's letter is shorter than its array.
        const char* letter = a_mark.letter.data();
        auto* baked = font->GetFontBaked(letterSize);
        if (!baked)
        {
            return;
        }
        const auto code = ReadCodePoint(letter);
        std::optional<ImVec2> origin;
        // Centered by the outline: the baked glyph's box rounds out to whole pixels by how far the outline
        // crosses into them, so a box-centered letter moves by its shape and by the size.
        if (const auto bounds = FindLetterBounds(code))
        {
            const auto baseline = std::round(baked->Ascent);
            origin = ImVec2{ std::round(a_min.x + (a_size - (bounds->right - bounds->left) * letterSize) * 0.5F
                                 - bounds->left * letterSize),
                std::round(a_min.y + (a_size - (bounds->bottom - bounds->top) * letterSize) * 0.5F
                           - bounds->top * letterSize - baseline) };
        }
        // A letter from a fallback font, by the baked box.
        else if (const auto* glyph = baked->FindGlyph(
                     static_cast<ImWchar>(code <= IM_UNICODE_CODEPOINT_MAX ? code : IM_UNICODE_CODEPOINT_INVALID)))
        {
            origin = ImVec2{ std::round(a_min.x + (a_size - (glyph->X1 - glyph->X0)) * 0.5F - glyph->X0),
                std::round(a_min.y + (a_size - (glyph->Y1 - glyph->Y0)) * 0.5F - glyph->Y0) };
        }
        if (origin)
        {
            a_list.AddText(font, letterSize, *origin, ImGui::ColorConvertFloat4ToU32(color), letter);
        }
    }

    void Mark(const ChatRoster::RoleMark& a_mark, float a_alpha)
    {
        const auto size = GetMarkSize(ImGui::GetFontSize());
        const auto height = ImGui::GetTextLineHeight();
        const auto cursor = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2{ size, height });
        DrawMark(*ImGui::GetWindowDrawList(), ImVec2{ cursor.x, cursor.y + std::floor((height - size) * 0.5F) }, size,
            a_mark, a_alpha * ImGui::GetStyle().Alpha);
    }

    void Name(const Multiplayer::Role& a_role)
    {
        const auto& style = ImGui::GetStyle();
        const auto padding = ChatStyle::GetChipPadding();
        const char* name = a_role.name.c_str();
        const char* nameEnd = name + a_role.name.size();
        const auto text = ImGui::CalcTextSize(name, nameEnd);
        const auto min = ImGui::GetCursorScreenPos();
        const ImVec2 max{ min.x + text.x + padding.x * 2.0F, min.y + text.y + padding.y * 2.0F };
        ImGui::Dummy(ImVec2{ max.x - min.x, max.y - min.y });
        const auto color = ToColor(a_role.color, style.Alpha);
        auto* drawList = ImGui::GetWindowDrawList();
        DrawChip(*drawList, min, max, color, style.FrameRounding, ImGui::GetFontSize());
        drawList->AddText(ImVec2{ min.x + padding.x, min.y + padding.y }, ImGui::ColorConvertFloat4ToU32(color), name,
            nameEnd);
    }
}
