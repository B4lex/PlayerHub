#include "ui/widgets/Controls.h"
#include "text/Text.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>

namespace UI::Controls
{
    namespace
    {
        constexpr float kToggleWidthFonts = 2.375F;
        constexpr float kToggleHeightFonts = 1.25F;
        constexpr float kToggleKnobInsetFonts = 0.1875F;
        constexpr float kToggleSeconds = 0.12F;
        constexpr float kIconFonts = 0.75F;
        // U+E009 and U+E00A of the logo font.
        constexpr const char* kCloseGlyph = "\xEE\x80\x89";
        constexpr const char* kResetGlyph = "\xEE\x80\x8A";

        constexpr ImVec4 kToggleOffKnob{ 0.72F, 0.71F, 0.69F, 1.0F };
        constexpr ImVec4 kBlack{ 0.0F, 0.0F, 0.0F, 1.0F };
        constexpr float kPrimaryPressedShade = 0.14F;
        constexpr ImVec4 kDangerPressed{ 0.80F, 0.30F, 0.26F, 1.0F };

        int KeepTab(ImGuiInputTextCallbackData*)
        {
            return 0;
        }

        constexpr ImVec4 Mix(const ImVec4& a_from, const ImVec4& a_to, float a_amount)
        {
            return ImVec4{ a_from.x + (a_to.x - a_from.x) * a_amount, a_from.y + (a_to.y - a_from.y) * a_amount,
                a_from.z + (a_to.z - a_from.z) * a_amount, a_from.w + (a_to.w - a_from.w) * a_amount };
        }

        bool FilledButton(const char* a_label, const ImVec2& a_size, const ImVec4& a_fill, const ImVec4& a_pressed,
            const ImVec4& a_text)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ a_fill.x, a_fill.y, a_fill.z, 0.90F });
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, a_fill);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, a_pressed);
            ImGui::PushStyleColor(ImGuiCol_Text, a_text);
            const bool pressed = ImGui::Button(a_label, a_size);
            ImGui::PopStyleColor(4);
            return pressed;
        }
    }

    float GetToggleWidth()
    {
        return std::floor(kToggleWidthFonts * ImGui::GetFontSize());
    }

    bool Toggle(const char* a_id, bool& a_value)
    {
        const auto fontSize = ImGui::GetFontSize();
        const auto frameHeight = ImGui::GetFrameHeight();
        const ImVec2 size{ GetToggleWidth(), std::floor(kToggleHeightFonts * fontSize) };
        const auto position = ImGui::GetCursorScreenPos();

        const bool pressed = ImGui::InvisibleButton(a_id, ImVec2{ size.x, frameHeight });
        if (pressed)
        {
            a_value = !a_value;
        }

        auto& place = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), a_value ? 1.0F : 0.0F);
        const float step = ImGui::GetIO().DeltaTime / kToggleSeconds;
        place = a_value ? (std::min)(place + step, 1.0F) : (std::max)(place - step, 0.0F);

        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 trackMin{ position.x, position.y + std::floor((frameHeight - size.y) * 0.5F) };
        const ImVec2 trackMax{ trackMin.x + size.x, trackMin.y + size.y };
        const float radius = size.y * 0.5F;
        const auto offTrack = Theme::Tint(hovered ? 0.20F : 0.13F);
        const auto onTrack = hovered ? Theme::GetAccent() : Theme::Accent(0.88F);
        auto& list = *ImGui::GetWindowDrawList();
        list.AddRectFilled(trackMin, trackMax, ImGui::GetColorU32(Mix(offTrack, onTrack, place)), radius);
        list.AddCircleFilled(ImVec2{ trackMin.x + radius + (size.x - size.y) * place, trackMin.y + radius },
            radius - kToggleKnobInsetFonts * fontSize,
            ImGui::GetColorU32(Mix(kToggleOffKnob, ImGui::GetStyleColorVec4(ImGuiCol_Text), place)));
        return pressed;
    }

    bool PrimaryButton(const char* a_label, const ImVec2& a_size)
    {
        const auto accent = Theme::GetAccent();
        return FilledButton(a_label, a_size, accent, Mix(accent, kBlack, kPrimaryPressedShade), Theme::GetOnAccent());
    }

    bool DangerButton(const char* a_label, const ImVec2& a_size)
    {
        return FilledButton(a_label, a_size, Theme::kNegative, kDangerPressed, Theme::kOnState);
    }

    bool IconButton(const char* a_id, Icon a_icon)
    {
        const auto size = ImGui::GetFrameHeight();
        const auto position = ImGui::GetCursorScreenPos();
        const bool pressed = ImGui::InvisibleButton(a_id, ImVec2{ size, size });
        const bool hovered = ImGui::IsItemHovered();

        auto& list = *ImGui::GetWindowDrawList();
        if (hovered)
        {
            list.AddRectFilled(position, ImVec2{ position.x + size, position.y + size },
                ImGui::GetColorU32(Theme::Tint(ImGui::IsItemActive() ? 0.16F : 0.10F)),
                ImGui::GetStyle().FrameRounding);
        }
        if (auto* const font = Fonts::GetLogo())
        {
            const auto glyph = std::round(kIconFonts * ImGui::GetFontSize());
            const auto inset = std::floor((size - glyph) * 0.5F);
            list.AddText(font, glyph, ImVec2{ position.x + inset, position.y + inset },
                ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled),
                a_icon == Icon::kClose ? kCloseGlyph : kResetGlyph);
        }
        return pressed;
    }

    bool SearchField(const char* a_id, std::span<char> a_buffer)
    {
        ImGui::SetNextItemWidth(-FLT_MIN);
        return ImGui::InputTextWithHint(
            a_id, Text::GetCString(Text::ID::kSearchHint), a_buffer.data(), a_buffer.size(),
            ImGuiInputTextFlags_CallbackCompletion, &KeepTab);
    }

    void MutedTextWrapped(const char* a_text)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", a_text);
        ImGui::PopStyleColor();
    }

    void AccentedText(std::string_view a_format, std::span<const std::string_view> a_arguments)
    {
        std::array<Text::Piece, 8> pieces;
        const auto count = Text::Split(a_format, pieces);
        for (std::size_t i = 0; i < count; ++i)
        {
            if (i > 0)
            {
                ImGui::SameLine(0.0F, 0.0F);
            }
            const auto argument = static_cast<std::size_t>(pieces[i].argument);
            const bool accented = pieces[i].argument >= 0 && argument < a_arguments.size();
            const auto text = accented ? a_arguments[argument] : pieces[i].text;
            if (accented)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::GetAccent());
            }
            ImGui::TextUnformatted(text.data(), text.data() + text.size());
            if (accented)
            {
                ImGui::PopStyleColor();
            }
        }
    }

    void StripedBox(ImDrawList& a_list, const ImVec2& a_min, const ImVec2& a_max, float a_strip, ImU32 a_stripColor,
        ImU32 a_bodyColor, float a_rounding)
    {
        // ImGui clamps a rect's rounding to its width, so a strip drawn as its own rect gets near-square
        // corners that stick out of the body's curve. Both halves are clipped from one rounded rect instead;
        // the seam is a whole pixel because backends truncate the scissor, and the outer clip edges leave
        // room for the anti-aliased fringe.
        const auto seam = std::floor(a_min.x + a_strip);
        a_list.PushClipRect(ImVec2{ a_min.x - 1.0F, a_min.y - 1.0F }, ImVec2{ seam, a_max.y + 1.0F }, true);
        a_list.AddRectFilled(a_min, a_max, a_stripColor, a_rounding);
        a_list.PopClipRect();
        a_list.PushClipRect(ImVec2{ seam, a_min.y - 1.0F }, ImVec2{ a_max.x + 1.0F, a_max.y + 1.0F }, true);
        a_list.AddRectFilled(a_min, a_max, a_bodyColor, a_rounding);
        a_list.PopClipRect();
    }
}
