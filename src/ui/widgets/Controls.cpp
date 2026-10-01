#include "ui/widgets/Controls.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace UI::Controls
{
    namespace
    {
        // In font sizes, so the controls follow the overlay scale.
        constexpr float kToggleWidthFonts = 2.375F;
        constexpr float kToggleHeightFonts = 1.25F;
        constexpr float kToggleKnobInsetFonts = 0.1875F;
        constexpr float kToggleSeconds = 0.12F;
        constexpr float kIconLineFonts = 0.09375F;
        // Of the button's size.
        constexpr float kCloseReach = 0.17F;
        constexpr float kResetRadius = 0.21F;
        constexpr float kResetHead = 0.15F;

        constexpr ImVec4 kToggleOffKnob{ 0.72F, 0.71F, 0.69F, 1.0F };
        constexpr ImVec4 kPrimaryPressed{ 0.86F, 0.48F, 0.0F, 1.0F };
        constexpr ImVec4 kDangerPressed{ 0.80F, 0.30F, 0.26F, 1.0F };

        constexpr ImVec4 Mix(const ImVec4& a_from, const ImVec4& a_to, float a_amount)
        {
            return ImVec4{ a_from.x + (a_to.x - a_from.x) * a_amount, a_from.y + (a_to.y - a_from.y) * a_amount,
                a_from.z + (a_to.z - a_from.z) * a_amount, a_from.w + (a_to.w - a_from.w) * a_amount };
        }

        bool FilledButton(const char* a_label, const ImVec2& a_size, const ImVec4& a_fill, const ImVec4& a_pressed)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4{ a_fill.x, a_fill.y, a_fill.z, 0.90F });
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, a_fill);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, a_pressed);
            ImGui::PushStyleColor(ImGuiCol_Text, Theme::kOnAccent);
            const bool pressed = ImGui::Button(a_label, a_size);
            ImGui::PopStyleColor(4);
            return pressed;
        }

        void DrawClose(ImDrawList& a_list, const ImVec2& a_center, float a_size, ImU32 a_color, float a_thickness)
        {
            const float reach = a_size * kCloseReach;
            a_list.AddLine(ImVec2{ a_center.x - reach, a_center.y - reach },
                ImVec2{ a_center.x + reach, a_center.y + reach }, a_color, a_thickness);
            a_list.AddLine(ImVec2{ a_center.x - reach, a_center.y + reach },
                ImVec2{ a_center.x + reach, a_center.y - reach }, a_color, a_thickness);
        }

        // An open circle that ends, at the upper right, in an arrowhead heading
        // back around it.
        void DrawReset(ImDrawList& a_list, const ImVec2& a_center, float a_size, ImU32 a_color, float a_thickness)
        {
            constexpr float kHeadAngle = -0.25F * std::numbers::pi_v<float>;
            constexpr float kTailAngle = 1.30F * std::numbers::pi_v<float>;
            const float radius = a_size * kResetRadius;
            const float head = a_size * kResetHead;
            a_list.PathArcTo(a_center, radius, kHeadAngle, kTailAngle);
            a_list.PathStroke(a_color, ImDrawFlags_None, a_thickness);

            const ImVec2 outward{ std::cos(kHeadAngle), std::sin(kHeadAngle) };
            const ImVec2 forward{ outward.y, -outward.x };
            const ImVec2 tip{ a_center.x + outward.x * radius, a_center.y + outward.y * radius };
            const ImVec2 base{ tip.x - forward.x * head * 0.25F, tip.y - forward.y * head * 0.25F };
            a_list.AddTriangleFilled(ImVec2{ tip.x + forward.x * head, tip.y + forward.y * head },
                ImVec2{ base.x + outward.x * head * 0.8F, base.y + outward.y * head * 0.8F },
                ImVec2{ base.x - outward.x * head * 0.8F, base.y - outward.y * head * 0.8F }, a_color);
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

        // The knob's place between off and on, which follows the value.
        auto& place = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), a_value ? 1.0F : 0.0F);
        const float step = ImGui::GetIO().DeltaTime / kToggleSeconds;
        place = a_value ? (std::min)(place + step, 1.0F) : (std::max)(place - step, 0.0F);

        const bool hovered = ImGui::IsItemHovered();
        const ImVec2 trackMin{ position.x, position.y + std::floor((frameHeight - size.y) * 0.5F) };
        const ImVec2 trackMax{ trackMin.x + size.x, trackMin.y + size.y };
        const float radius = size.y * 0.5F;
        const auto offTrack = Theme::Tint(hovered ? 0.20F : 0.13F);
        const auto onTrack = hovered ? Theme::kAccent : Theme::Accent(0.88F);
        auto& list = *ImGui::GetWindowDrawList();
        list.AddRectFilled(trackMin, trackMax, ImGui::GetColorU32(Mix(offTrack, onTrack, place)), radius);
        list.AddCircleFilled(ImVec2{ trackMin.x + radius + (size.x - size.y) * place, trackMin.y + radius },
            radius - kToggleKnobInsetFonts * fontSize,
            ImGui::GetColorU32(Mix(kToggleOffKnob, ImGui::GetStyleColorVec4(ImGuiCol_Text), place)));
        return pressed;
    }

    bool PrimaryButton(const char* a_label, const ImVec2& a_size)
    {
        return FilledButton(a_label, a_size, Theme::kAccent, kPrimaryPressed);
    }

    bool DangerButton(const char* a_label, const ImVec2& a_size)
    {
        return FilledButton(a_label, a_size, Theme::kNegative, kDangerPressed);
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
        const auto color = ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
        const float thickness = (std::max)(kIconLineFonts * ImGui::GetFontSize(), 1.0F);
        const ImVec2 center{ position.x + size * 0.5F, position.y + size * 0.5F };
        switch (a_icon)
        {
            case Icon::kClose:
                DrawClose(list, center, size, color, thickness);
                break;
            case Icon::kReset:
                DrawReset(list, center, size, color, thickness);
                break;
        }
        return pressed;
    }
}
