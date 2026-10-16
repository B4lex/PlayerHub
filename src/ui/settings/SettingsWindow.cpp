#include "ui/settings/SettingsWindow.h"
#include "config/PluginConfig.h"
#include "config/SettingsSchema.h"
#include "protocol/Session.h"
#include "text/Text.h"
#include "text/Utf8Case.h"
#include "ui/Fonts.h"
#include "ui/MenuFrame.h"
#include "ui/Theme.h"
#include "ui/WindowManager.h"
#include "ui/settings/ConnectionControls.h"
#include "ui/widgets/Controls.h"
#include "ui/widgets/ItemPicker.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace UI
{
    namespace
    {
        constexpr ImVec2 kSizeFonts{ 54.0F, 36.0F };
        constexpr ImVec2 kMinimumSizeFonts{ 40.0F, 20.0F };
        constexpr float kSidebarWidthFonts = 11.5F;
        constexpr ImVec2 kSidebarPaddingFonts{ 0.5F, 1.25F };
        constexpr float kNavigationHeightFonts = 2.125F;
        constexpr float kNavigationInsetFonts = 0.875F;
        constexpr float kNavigationGapFonts = 0.125F;
        constexpr float kNavigationMarkFonts = 0.2F;
        constexpr float kNavigationIdleAlpha = 0.68F;
        constexpr ImVec2 kPagePaddingFonts{ 1.5F, 1.125F };
        constexpr float kCloseInsetFonts = 0.75F;
        constexpr float kFooterGapFonts = 0.75F;
        constexpr float kFooterButtonWidthFonts = 5.5F;
        constexpr float kTooltipWidthFonts = 22.0F;
        constexpr float kSwatchGapFonts = 0.25F;
        constexpr float kSwatchRingFonts = 0.125F;
        constexpr float kSwatchIconFonts = 0.09375F;
        constexpr float kPickerWidthFonts = 13.0F;
        constexpr float kCaptureRingFonts = 0.125F;
        constexpr float kTitleScale = 1.375F;
        constexpr float kBrandScale = 1.125F;
        constexpr float kGroupScale = 0.8125F;
        constexpr float kControlWidth = 0.58F;

        constexpr std::string_view kConnectionSection = "Connection";
        constexpr std::string_view kDisplayNameKey = "DisplayName";
        constexpr std::string_view kServerEndpointKey = "ServerEndpoint";
        constexpr std::string_view kStagedSection = "DefaultOutfit";
        constexpr std::string_view kUISection = "UI";
        constexpr std::string_view kControlsSection = "Controls";
        constexpr std::string_view kAdvancedSection = "Advanced";
        constexpr std::string_view kDefaultsSections[]{ kUISection, kControlsSection, kAdvancedSection };

        constexpr std::string_view kConfigFileName = "PlayerHub.ini";
        constexpr std::uint32_t kEscapeScanCode = RE::BSKeyboardDevice::Key::kEscape;
        constexpr std::uint32_t kBackspaceScanCode = RE::BSKeyboardDevice::Key::kBackspace;
        // U+E000 and U+E001 of the logo font: the letters and the bar.
        constexpr const char* kLogoLetters = "\xEE\x80\x80";
        constexpr const char* kLogoBar = "\xEE\x80\x81";

        constexpr ImGuiWindowFlags kFixedFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse
            | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoBackground | kFixedFlags;

        int FilterDisplayName(ImGuiInputTextCallbackData* a_data)
        {
            if (a_data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter)
            {
                const auto character = a_data->EventChar;
                return character < 0x80 && Multiplayer::IsDisplayNameCharacter(static_cast<char>(character)) ? 0 : 1;
            }
            // The std::string overload has no buffer limit, so a paste past the limit is cut after the edit.
            constexpr auto maximum = static_cast<int>(Multiplayer::kMaximumDisplayNameLength);
            if (a_data->BufTextLen > maximum)
            {
                a_data->DeleteChars(maximum, a_data->BufTextLen - maximum);
            }
            return 0;
        }

        void DrawText(std::string_view a_text)
        {
            ImGui::TextUnformatted(a_text.data(), a_text.data() + a_text.size());
        }

        void DrawMutedText(std::string_view a_text)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            DrawText(a_text);
            ImGui::PopStyleColor();
        }

        bool DrawNavigationItem(std::string_view a_id, const char* a_label, bool a_selected)
        {
            const auto fontSize = ImGui::GetFontSize();
            const auto position = ImGui::GetCursorScreenPos();
            const ImVec2 size{ ImGui::GetContentRegionAvail().x, std::floor(kNavigationHeightFonts * fontSize) };
            ImGui::PushID(a_id.data(), a_id.data() + a_id.size());
            const bool pressed = ImGui::InvisibleButton("##section", size);
            ImGui::PopID();
            const bool hovered = ImGui::IsItemHovered();

            auto& list = *ImGui::GetWindowDrawList();
            const ImVec2 end{ position.x + size.x, position.y + size.y };
            const auto rounding = ImGui::GetStyle().FrameRounding;
            if (a_selected)
            {
                Controls::StripedBox(list, position, end, std::floor(kNavigationMarkFonts * fontSize),
                    ImGui::GetColorU32(Theme::GetAccent()), ImGui::GetColorU32(Theme::Accent(0.16F)), rounding);
            }
            else if (hovered)
            {
                list.AddRectFilled(position, end, ImGui::GetColorU32(Theme::Tint(0.06F)), rounding);
            }
            list.AddText(ImVec2{ position.x + std::floor(kNavigationInsetFonts * fontSize),
                             position.y + std::floor((size.y - fontSize) * 0.5F) },
                ImGui::GetColorU32(ImGuiCol_Text, a_selected || hovered ? 1.0F : kNavigationIdleAlpha), a_label);
            return pressed;
        }

        std::uint32_t ToRgb(const float (&a_color)[3])
        {
            const auto channel = [](float a_value) {
                return static_cast<std::uint32_t>(std::lround(std::clamp(a_value, 0.0F, 1.0F) * 255.0F));
            };
            return (channel(a_color[0]) << 16) | (channel(a_color[1]) << 8) | channel(a_color[2]);
        }

        bool DrawSwatch(const char* a_id, const ImVec4* a_color, float a_size, bool a_selected)
        {
            const auto frameHeight = ImGui::GetFrameHeight();
            const auto position = ImGui::GetCursorScreenPos();
            const bool pressed = ImGui::InvisibleButton(a_id, ImVec2{ a_size, frameHeight });
            const bool hovered = ImGui::IsItemHovered();
            if (hovered)
            {
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            }

            auto& list = *ImGui::GetWindowDrawList();
            const auto fontSize = ImGui::GetFontSize();
            const auto rounding = ImGui::GetStyle().FrameRounding;
            const ImVec2 min{ position.x, position.y + std::floor((frameHeight - a_size) * 0.5F) };
            const ImVec2 max{ min.x + a_size, min.y + a_size };
            const auto ring = (std::max)(std::floor(kSwatchRingFonts * fontSize), 1.0F);
            const auto fill
                = a_color ? *a_color : ImGui::GetStyleColorVec4(hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
            if (a_selected)
            {
                const auto inset = ring * 2.0F;
                list.AddRect(min, max, ImGui::GetColorU32(ImGuiCol_Text), rounding, ImDrawFlags_None, ring);
                list.AddRectFilled(ImVec2{ min.x + inset, min.y + inset }, ImVec2{ max.x - inset, max.y - inset },
                    ImGui::GetColorU32(fill), (std::max)(rounding - inset, 0.0F));
            }
            else
            {
                list.AddRectFilled(min, max, ImGui::GetColorU32(fill), rounding);
                if (hovered && a_color)
                {
                    list.AddRect(
                        min, max, ImGui::GetColorU32(Theme::Tint(0.60F)), rounding, ImDrawFlags_None, ring * 0.5F);
                }
            }
            if (!a_color)
            {
                const auto reach = std::floor(a_size * 0.25F);
                const auto thickness = (std::max)(kSwatchIconFonts * fontSize, 1.0F);
                const ImVec2 center{ std::floor((min.x + max.x) * 0.5F), std::floor((min.y + max.y) * 0.5F) };
                const auto color = ImGui::GetColorU32(hovered ? ImGuiCol_Text : ImGuiCol_TextDisabled);
                list.AddLine(
                    ImVec2{ center.x - reach, center.y }, ImVec2{ center.x + reach, center.y }, color, thickness);
                list.AddLine(
                    ImVec2{ center.x, center.y - reach }, ImVec2{ center.x, center.y + reach }, color, thickness);
            }
            return pressed;
        }

        class SettingsEditor final : public Config::SettingsVisitor
        {
        public:
            void Reset()
            {
                fields.clear();
                itemPicker.Reset();
                StopKeyCapture();
            }

            void FocusConnectionField()
            {
                openSection = kConnectionSection;
                focusConnectionField = true;
            }

            [[nodiscard]] bool IsStagedSectionOpen() const { return stagedSectionOpen; }

            [[nodiscard]] std::string_view GetDefaultsSection() const
            {
                return defaultsSectionOpen ? openSection : std::string_view{};
            }

            [[nodiscard]] bool IsEditing() const { return editing; }

            [[nodiscard]] bool TakeConnectRequest() { return std::exchange(connectRequested, false); }

            [[nodiscard]] float GetScrollbarWidth() const { return scrollbarWidth; }

            void DrawPage(Config::PluginConfig& a_config, const ImVec2& a_padding, float a_buttonFooterHeight,
                float a_plainFooterHeight)
            {
                padding = a_padding;
                buttonFooterHeight = a_buttonFooterHeight;
                plainFooterHeight = a_plainFooterHeight;
                stagedSectionOpen = false;
                defaultsSectionOpen = false;
                editing = false;
                sections.clear();
                bindings.clear();
                pressedKey = {};
                Config::Describe(a_config, *this);
                EndSection();
                // After the pass, so the rows a click lands on are where the player saw them:
                // a rejection's line under a capturing row goes when its capture ends.
                if (!pressedKey.empty() && pressedKey != capturingKey)
                {
                    StartKeyCapture(pressedKey);
                }
                else if (!capturingKey.empty() && IsAnyMouseClicked())
                {
                    StopKeyCapture();
                }
                ResolveKeyCapture();
            }

            [[nodiscard]] bool CapturesKeys() const { return keyCapture.active.load(std::memory_order_acquire); }

            void OnCapturedKey(std::uint32_t a_scanCode)
            {
                // Keeps the first key until the overlay takes it; later ones are dropped.
                std::uint32_t none = 0;
                keyCapture.pressed.compare_exchange_strong(none, a_scanCode, std::memory_order_acq_rel);
            }

            void DrawNavigation()
            {
                ImGui::PushStyleVar(
                    ImGuiStyleVar_ItemSpacing, ImVec2{ 0.0F, std::floor(kNavigationGapFonts * ImGui::GetFontSize()) });
                for (const auto& section : sections)
                {
                    if (DrawNavigationItem(section.name, Text::GetCString(section.title), section.name == openSection))
                    {
                        openSection = section.name;
                    }
                }
                ImGui::PopStyleVar();
            }

            void Section(std::string_view a_name, Text::Id a_title, Text::Id a_comment) override
            {
                EndSection();
                hideNext = false;
                sections.push_back(SectionEntry{ .name = a_name, .title = a_title });
                if (openSection.empty())
                {
                    openSection = a_name;
                }
                if (a_name != openSection)
                {
                    return;
                }
                sectionOpen = true;
                sectionEmpty = true;
                stagedSectionOpen = a_name == kStagedSection;
                defaultsSectionOpen = std::ranges::contains(kDefaultsSections, a_name);
                ImGui::PushID(a_name.data(), a_name.data() + a_name.size());

                const auto& style = ImGui::GetStyle();
                ImGui::SetCursorPos(padding);
                ImGui::PushFont(Fonts::GetHeading(), style.FontSizeBase * kTitleScale);
                DrawText(Text::Get(a_title));
                ImGui::PopFont();
                if (a_comment != Text::Id::kNone)
                {
                    ImGui::SetCursorPosX(padding.x);
                    FormatNote(Text::Get(a_comment));
                    ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - padding.x);
                    DrawMutedText(note);
                    ImGui::PopTextWrapPos();
                }
                ImGui::Dummy(ImVec2{ 0.0F, std::floor(style.ItemSpacing.y * 0.5F) });

                const auto footerHeight
                    = stagedSectionOpen || defaultsSectionOpen ? buttonFooterHeight : plainFooterHeight;
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ padding.x, 0.0F });
                ImGui::BeginChild("##Content", ImVec2{ 0.0F, -footerHeight }, ImGuiChildFlags_AlwaysUseWindowPadding);
                ImGui::PopStyleVar();
            }

            void ShowNextIf(bool a_shown) override { hideNext = !a_shown; }

            void Comment(Text::Id a_comment) override
            {
                if (!sectionOpen)
                {
                    return;
                }
                const auto& style = ImGui::GetStyle();
                if (!sectionEmpty)
                {
                    ImGui::Dummy(ImVec2{ 0.0F, style.ItemSpacing.y });
                }
                sectionEmpty = false;
                note.clear();
                Utf8Case::AppendUpper(note, Text::Get(a_comment));
                ImGui::PushFont(Fonts::GetHeading(), style.FontSizeBase * kGroupScale);
                DrawMutedText(note);
                ImGui::PopFont();
            }

            void Bool(std::string_view a_key, Text::Id a_label, bool& a_value, Text::Id a_comment) override
            {
                if (!BeginRow(a_key, a_label, a_comment))
                {
                    return;
                }
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + controlWidth - Controls::GetToggleWidth());
                Controls::Toggle("##value", a_value);
                EndRow();
            }

            void Unsigned(std::string_view a_key, Text::Id a_label, std::uint32_t& a_value, Text::Id a_comment,
                std::uint32_t a_maximum, Text::Id a_zeroName, std::uint32_t a_step) override
            {
                if (!BeginRow(a_key, a_label, a_comment))
                {
                    return;
                }
                // A format without a conversion is drawn as it is, so zero shows its name.
                label.assign(a_value == 0 && a_zeroName != Text::Id::kNone ? Text::Get(a_zeroName) : "%u");
                constexpr std::uint32_t kMinimum = 0;
                if (ImGui::SliderScalar("##value", ImGuiDataType_U32, &a_value, &kMinimum, &a_maximum, label.c_str(),
                        ImGuiSliderFlags_AlwaysClamp))
                {
                    a_value = (std::min)((a_value + a_step / 2) / a_step * a_step, a_maximum);
                }
                // Applied on release, so a drag does not save the file in every frame.
                editing = editing || ImGui::IsItemActive();
                EndRow();
            }

            void Percent(std::string_view a_key, Text::Id a_label, std::uint32_t& a_value, Text::Id a_comment,
                std::uint32_t a_minimum, std::uint32_t a_maximum, std::uint32_t a_step) override
            {
                if (!BeginRow(a_key, a_label, a_comment))
                {
                    return;
                }
                if (ImGui::SliderScalar("##value", ImGuiDataType_U32, &a_value, &a_minimum, &a_maximum, "%u%%",
                        ImGuiSliderFlags_AlwaysClamp))
                {
                    a_value = a_minimum + (a_value - a_minimum + a_step / 2) / a_step * a_step;
                }
                // Applied on release: a frame resized under the cursor would move the slider.
                editing = editing || ImGui::IsItemActive();
                EndRow();
            }

            void String(std::string_view a_key, Text::Id a_label, std::string& a_value, Text::Id a_comment,
                bool (*a_isValid)(std::string_view)) override
            {
                if (sectionOpen && openSection == kConnectionSection && a_key == kDisplayNameKey)
                {
                    DrawConnectionBar(a_key, a_label, a_value, a_comment, a_isValid);
                    return;
                }
                const bool endpoint = a_key == kServerEndpointKey;
                if (!BeginRow(a_key, a_label, a_comment, !endpoint))
                {
                    return;
                }
                auto& field = GetField([&a_value] { return a_value; });
                if (DrawTextField(field, endpoint ? Text::GetCString(Text::Id::kSettingsServerEndpointHint) : nullptr))
                {
                    field.valid = a_isValid(field.text);
                }
                if (ImGui::IsItemDeactivatedAfterEdit() && field.valid)
                {
                    a_value = field.text;
                }
                if (!field.valid)
                {
                    DrawTooltip(Text::Get(a_comment));
                    if (endpoint)
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, Theme::kNegative);
                        ImGui::PushTextWrapPos(0.0F);
                        DrawText(Text::Get(Text::Id::kSettingsServerEndpointInvalid));
                        ImGui::PopTextWrapPos();
                        ImGui::PopStyleColor();
                    }
                }
                EndRow();
            }

            void Form(std::string_view a_key, Text::Id a_label, Multiplayer::FormReference& a_value,
                const Multiplayer::FormReference& a_default, const Forms::ItemFilter& a_filter) override
            {
                if (!BeginRow(a_key, a_label, Text::Id::kNone))
                {
                    return;
                }
                itemPicker.Draw("##value", a_value, a_default, a_filter);
                EndRow();
            }

            void Choice(std::string_view a_key, Text::Id a_label, std::uint32_t& a_value, Text::Id a_comment,
                std::span<const std::string_view>, std::span<const Text::Id> a_optionLabels,
                std::uint32_t (*a_unset)()) override
            {
                if (!BeginRow(a_key, a_label, a_comment))
                {
                    return;
                }
                const auto shown = a_value >= a_optionLabels.size() && a_unset ? a_unset() : a_value;
                const auto* preview = shown < a_optionLabels.size() ? Text::GetCString(a_optionLabels[shown]) : "";
                if (ImGui::BeginCombo("##value", preview))
                {
                    for (std::uint32_t index = 0; index < a_optionLabels.size(); ++index)
                    {
                        const bool selected = index == shown;
                        ImGui::PushID(static_cast<int>(index));
                        const bool pressed = ImGui::Selectable(Text::GetCString(a_optionLabels[index]), selected);
                        ImGui::PopID();
                        if (pressed)
                        {
                            a_value = index;
                        }
                        if (selected)
                        {
                            ImGui::SetItemDefaultFocus();
                        }
                    }
                    ImGui::EndCombo();
                }
                EndRow();
            }

            void Color(std::string_view a_key, Text::Id a_label, std::uint32_t& a_value, Text::Id a_comment,
                std::span<const std::uint32_t> a_swatches) override
            {
                if (!BeginRow(a_key, a_label, a_comment))
                {
                    return;
                }
                const auto count = static_cast<float>(a_swatches.size() + 1);
                const auto minimumGap = std::floor(kSwatchGapFonts * ImGui::GetFontSize());
                const auto size = std::floor(
                    (std::min)(ImGui::GetFrameHeight(), (controlWidth - minimumGap * (count - 1.0F)) / count));
                const auto gap = (controlWidth - size * count) / (count - 1.0F);
                const auto left = ImGui::GetCursorPosX();
                bool custom = true;
                for (std::size_t index = 0; index < a_swatches.size(); ++index)
                {
                    const bool selected = a_swatches[index] == a_value;
                    custom = custom && !selected;
                    const auto color = Theme::FromRgb(a_swatches[index]);
                    ImGui::SetCursorPosX(std::floor(left + static_cast<float>(index) * (size + gap)));
                    ImGui::PushID(static_cast<int>(index));
                    if (DrawSwatch("##swatch", &color, size, selected))
                    {
                        a_value = a_swatches[index];
                    }
                    DrawColorTooltip({}, a_swatches[index]);
                    ImGui::PopID();
                    ImGui::SameLine();
                }

                ImGui::SetCursorPosX(left + controlWidth - size);
                const auto color = Theme::FromRgb(a_value);
                if (DrawSwatch("##custom", custom ? &color : nullptr, size, custom))
                {
                    ImGui::OpenPopup("##picker");
                }
                if (!ImGui::IsPopupOpen("##picker"))
                {
                    DrawColorTooltip(Text::Get(Text::Id::kSettingsCustomColor), custom ? std::optional{ a_value } : std::nullopt);
                }
                const auto anchor = ImGui::GetItemRectMax();
                ImGui::SetNextWindowPos(ImVec2{ anchor.x, anchor.y + ImGui::GetStyle().ItemSpacing.y },
                    ImGuiCond_Appearing, ImVec2{ 1.0F, 0.0F });
                if (ImGui::BeginPopup("##picker"))
                {
                    constexpr ImGuiColorEditFlags kPickerFlags = ImGuiColorEditFlags_NoAlpha
                        | ImGuiColorEditFlags_NoOptions | ImGuiColorEditFlags_NoLabel
                        | ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview
                        | ImGuiColorEditFlags_PickerHueBar | ImGuiColorEditFlags_DisplayHex
                        | ImGuiColorEditFlags_InputRGB | ImGuiColorEditFlags_Uint8;
                    // What was active last frame takes Escape first.
                    const bool escapeClaimed = ImGui::IsAnyItemActive();
                    float rgb[3]{ color.x, color.y, color.z };
                    ImGui::SetNextItemWidth(std::floor(kPickerWidthFonts * ImGui::GetFontSize()));
                    if (ImGui::ColorPicker3("##value", rgb, kPickerFlags))
                    {
                        a_value = ToRgb(rgb);
                    }
                    // Applied on release or when the hex field's editing ends, so a drag does
                    // not save the file in every frame.
                    editing = editing || ImGui::IsItemActive();
                    if (!escapeClaimed && ImGui::IsKeyPressed(ImGuiKey_Escape, false))
                    {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
                EndRow();
            }

            void Key(std::string_view a_key, Text::Id a_label, std::uint32_t& a_value, Text::Id a_comment,
                std::uint32_t a_cleared) override
            {
                bindings.push_back(
                    KeyBinding{ .name = a_key, .label = a_label, .value = &a_value, .cleared = a_cleared });
                if (!BeginRow(a_key, a_label, a_comment))
                {
                    return;
                }
                const bool capturing = capturingKey == a_key;
                if (DrawKeyButton(
                        capturing ? Text::Get(Text::Id::kSettingsKeyPrompt) : Config::FormatKeyLabel(a_value), capturing))
                {
                    pressedKey = a_key;
                }
                if (capturing && captureRejected)
                {
                    ImGui::SetCursorPosX(controlLeft);
                    ImGui::PushTextWrapPos(controlLeft + controlWidth);
                    ImGui::PushStyleColor(ImGuiCol_Text, Theme::kNegative);
                    DrawText(captureNote);
                    ImGui::PopStyleColor();
                    ImGui::PopTextWrapPos();
                }
                EndRow();
            }

            void Layout(std::string_view, std::uint32_t&, std::string_view, std::uint32_t, std::uint32_t) override {}

        private:
            struct TextField
            {
                std::string text;
                bool valid{ true };
            };

            struct SectionEntry
            {
                std::string_view name;
                Text::Id title;
            };

            struct KeyBinding
            {
                std::string_view name;
                Text::Id label;
                std::uint32_t* value;
                std::uint32_t cleared;
            };

            // Read and written by the input thread too.
            struct KeyCapture
            {
                std::atomic_bool active{ false };
                std::atomic_uint32_t pressed{ 0 };
            };

            static bool IsAnyMouseClicked()
            {
                return ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right)
                    || ImGui::IsMouseClicked(ImGuiMouseButton_Middle);
            }

            bool DrawKeyButton(std::string_view a_text, bool a_capturing)
            {
                const bool unbound = a_text.empty();
                label.assign(unbound ? Text::Get(Text::Id::kSettingsKeyUnbound) : a_text);
                label += "###value";
                if (a_capturing)
                {
                    ImGui::PushStyleColor(ImGuiCol_Button, Theme::Accent(0.16F));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Accent(0.22F));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Accent(0.28F));
                }
                if (unbound)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                }
                ImGui::Button(label.c_str(), ImVec2{ controlWidth, 0.0F });
                // On the press, which is when a click elsewhere cancels a capture.
                const bool activated = ImGui::IsItemActivated();
                if (unbound)
                {
                    ImGui::PopStyleColor();
                }
                if (a_capturing)
                {
                    ImGui::PopStyleColor(3);
                    const auto thickness = (std::max)(std::floor(kCaptureRingFonts * ImGui::GetFontSize()), 1.0F);
                    ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                        ImGui::GetColorU32(Theme::GetAccent()), ImGui::GetStyle().FrameRounding, ImDrawFlags_None,
                        thickness);
                }
                return activated;
            }

            void StartKeyCapture(std::string_view a_key)
            {
                capturingKey = a_key;
                captureRejected = false;
                keyCapture.pressed.store(0, std::memory_order_release);
                keyCapture.active.store(true, std::memory_order_release);
            }

            void StopKeyCapture()
            {
                keyCapture.active.store(false, std::memory_order_release);
                capturingKey = {};
                captureRejected = false;
            }

            void ResolveKeyCapture()
            {
                if (capturingKey.empty())
                {
                    return;
                }
                const auto scanCode = keyCapture.pressed.exchange(0, std::memory_order_acq_rel);
                if (scanCode == 0)
                {
                    return;
                }
                const auto binding = std::ranges::find(bindings, capturingKey, &KeyBinding::name);
                if (scanCode == kEscapeScanCode || binding == bindings.end())
                {
                    StopKeyCapture();
                    return;
                }
                const auto key = scanCode == kBackspaceScanCode ? binding->cleared : scanCode;
                if (*binding->value == key)
                {
                    StopKeyCapture();
                    return;
                }
                if (key != 0)
                {
                    const auto keyLabel = Config::FormatKeyLabel(key);
                    if (keyLabel.empty())
                    {
                        captureRejected = true;
                        captureNote.assign(Text::Get(Text::Id::kSettingsKeyReserved));
                        return;
                    }
                    const auto other = std::ranges::find(
                        bindings, key, [](const KeyBinding& a_binding) { return *a_binding.value; });
                    if (other != bindings.end())
                    {
                        captureRejected = true;
                        Text::FormatTo(captureNote, Text::Id::kSettingsKeyInUse, keyLabel, Text::Get(other->label));
                        return;
                    }
                }
                *binding->value = key;
                StopKeyCapture();
            }

            void DrawConnectionBar(std::string_view a_key, Text::Id a_label, std::string& a_value, Text::Id a_comment,
                bool (*a_isValid)(std::string_view))
            {
                sectionEmpty = false;
                ImGui::PushID(a_key.data(), a_key.data() + a_key.size());
                DrawText(Text::Get(a_label));

                auto& field = GetField([&a_value] { return a_value; });
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ConnectionControls::GetButtonWidth()
                    - ImGui::GetStyle().ItemSpacing.x);
                if (focusConnectionField)
                {
                    ImGui::SetKeyboardFocusHere();
                    focusConnectionField = false;
                }
                const auto* lock = ConnectionControls::GetDisplayNameLock();
                ImGui::BeginDisabled(lock != nullptr);
                if (DrawTextField(field, Text::GetCString(Text::Id::kSettingsDisplayNameHint),
                        ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_CallbackEdit, FilterDisplayName))
                {
                    field.valid = a_isValid(field.text);
                }
                ImGui::EndDisabled();
                const bool entered = field.valid && !field.text.empty();
                if (ImGui::IsItemDeactivatedAfterEdit() && field.valid)
                {
                    a_value = field.text;
                }
                // Enter leaves the field in the frame it applies the typed text.
                if (ImGui::IsItemDeactivated()
                    && (ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false)))
                {
                    connectRequested = ConnectionControls::CanConnect(entered);
                }
                if (lock)
                {
                    DrawTooltip(lock, ImGuiHoveredFlags_AllowWhenDisabled);
                }
                else
                {
                    DrawTooltip(Text::Get(a_comment));
                }

                ImGui::SameLine();
                ConnectionControls::DrawButton(entered);
                if (const auto* buttonLock = ConnectionControls::GetButtonLock())
                {
                    DrawTooltip(buttonLock, ImGuiHoveredFlags_AllowWhenDisabled);
                }
                ConnectionControls::DrawNotice(!field.valid);
                DrawConnectionDivider();
                ImGui::PopID();
            }

            void DrawConnectionDivider()
            {
                const auto spacing = ImGui::GetStyle().ItemSpacing.y;
                const auto gap = spacing * 2.0F + ImGui::GetTextLineHeight();
                const auto position = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddLine(position,
                    ImVec2{ position.x + ImGui::GetContentRegionAvail().x, position.y },
                    ImGui::GetColorU32(ImGuiCol_Separator), ImGui::GetStyle().WindowBorderSize);
                ImGui::Dummy(ImVec2{ 0.0F, gap - spacing });
            }

            void EndSection()
            {
                if (!sectionOpen)
                {
                    return;
                }
                ImGui::Dummy(ImVec2{ 0.0F, std::floor(ImGui::GetStyle().ItemSpacing.y * 0.5F) });
                scrollbarWidth = ImGui::GetScrollMaxY() > 0.0F ? ImGui::GetStyle().ScrollbarSize : 0.0F;
                ImGui::EndChild();
                ImGui::PopID();
                sectionOpen = false;
            }

            bool BeginRow(std::string_view a_key, Text::Id a_label, Text::Id a_comment, bool a_labeled = true)
            {
                const bool hidden = std::exchange(hideNext, false);
                if (!sectionOpen)
                {
                    return false;
                }
                if (hidden)
                {
                    // Forgets text in red, so a hidden field does not come back with it.
                    ImGui::PushID(a_key.data(), a_key.data() + a_key.size());
                    fields.erase(ImGui::GetID("##value"));
                    ImGui::PopID();
                    return false;
                }
                sectionEmpty = false;
                ImGui::PushID(a_key.data(), a_key.data() + a_key.size());
                const auto left = ImGui::GetCursorPosX();
                const auto width = ImGui::GetContentRegionAvail().x;
                if (!a_labeled)
                {
                    controlWidth = width;
                    controlLeft = left;
                    ImGui::SetNextItemWidth(controlWidth);
                    return true;
                }
                controlWidth = std::floor(width * kControlWidth);
                controlLeft = left + width - controlWidth;

                ImGui::AlignTextToFramePadding();
                DrawText(Text::Get(a_label));
                DrawTooltip(Text::Get(a_comment));
                ImGui::SameLine(controlLeft);
                ImGui::SetNextItemWidth(controlWidth);
                return true;
            }

            void EndRow() { ImGui::PopID(); }

            void FormatNote(std::string_view a_text)
            {
                note.assign(a_text);
                std::ranges::replace(note, '\n', ' ');
            }

            void DrawTooltip(std::string_view a_text, ImGuiHoveredFlags a_flags = ImGuiHoveredFlags_None)
            {
                if (a_text.empty() || !ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip | a_flags)
                    || !ImGui::BeginTooltip())
                {
                    return;
                }
                FormatNote(a_text);
                ImGui::PushTextWrapPos(kTooltipWidthFonts * ImGui::GetFontSize());
                DrawText(note);
                ImGui::PopTextWrapPos();
                ImGui::EndTooltip();
            }

            void DrawColorTooltip(std::string_view a_label, std::optional<std::uint32_t> a_color)
            {
                if (!ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip))
                {
                    return;
                }
                label.assign(a_label);
                if (a_color)
                {
                    label += label.empty() ? "#" : " #";
                    label += Config::FormatColor(*a_color);
                }
                DrawTooltip(label);
            }

            template <class Format>
            TextField& GetField(Format a_format)
            {
                auto [field, inserted] = fields.try_emplace(ImGui::GetID("##value"));
                if (inserted)
                {
                    field->second.text = a_format();
                }
                return field->second;
            }

            static bool DrawTextField(TextField& a_field, const char* a_hint, ImGuiInputTextFlags a_flags = 0,
                ImGuiInputTextCallback a_callback = nullptr)
            {
                if (!a_field.valid)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, Theme::kNegative);
                }
                const bool changed = a_hint
                    ? ImGui::InputTextWithHint("##value", a_hint, &a_field.text, a_flags, a_callback)
                    : ImGui::InputText("##value", &a_field.text, a_flags, a_callback);
                if (!a_field.valid)
                {
                    ImGui::PopStyleColor();
                }
                return changed;
            }

            std::unordered_map<ImGuiID, TextField> fields;
            ItemPicker itemPicker;
            KeyCapture keyCapture;
            // Collected in each pass, pointing into the configuration it draws.
            std::vector<KeyBinding> bindings;
            std::string captureNote;
            std::string_view capturingKey;
            std::string_view pressedKey;
            // Reused so drawing formats labels and notes without allocating.
            std::string label;
            std::string note;
            // Describe passes literals, so the views outlive the pass.
            std::vector<SectionEntry> sections;
            std::string_view openSection;
            ImVec2 padding;
            float buttonFooterHeight{ 0.0F };
            float plainFooterHeight{ 0.0F };
            float controlLeft{ 0.0F };
            float controlWidth{ 0.0F };
            float scrollbarWidth{ 0.0F };
            bool sectionOpen{ false };
            bool sectionEmpty{ false };
            bool stagedSectionOpen{ false };
            bool defaultsSectionOpen{ false };
            bool editing{ false };
            bool focusConnectionField{ false };
            bool connectRequested{ false };
            bool hideNext{ false };
            bool captureRejected{ false };
        };

        bool LiveSectionsMatch(const Config::PluginConfig& a_left, const Config::PluginConfig& a_right)
        {
            return a_left.connection == a_right.connection && a_left.ui == a_right.ui
                && a_left.controls == a_right.controls && a_left.advanced == a_right.advanced;
        }

        class SettingsWindow final : public Window
        {
        public:
            SettingsWindow()
                : defaults(Config::GetDefaultConfig())
            {
            }

            [[nodiscard]] bool IsFocusKey(std::uint32_t a_scanCode) const override
            {
                return a_scanCode == Config::Get()->controls.settingsKey;
            }

            [[nodiscard]] bool IsReleaseKey(std::uint32_t a_scanCode) const override { return IsFocusKey(a_scanCode); }

            [[nodiscard]] bool CapturesKeys() const override { return editor.CapturesKeys(); }

            void OnCapturedKey(std::uint32_t a_scanCode) override { editor.OnCapturedKey(a_scanCode); }

            [[nodiscard]] bool UsesMouse() const override { return true; }

            void Draw(const WindowInput& a_input) override
            {
                if (!a_input.focused)
                {
                    wasFocused = false;
                    return;
                }
                if (!wasFocused)
                {
                    Open();
                }

                // What was active or open last frame takes Escape first.
                const bool escapeClaimed
                    = ImGui::IsAnyItemActive() || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
                bool open = true;
                DrawWindow(open);
                if (!open || (!escapeClaimed && ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
                {
                    WindowManager::ReleaseFocus(*this);
                }
            }

        private:
            void Open()
            {
                wasFocused = true;
                focusWindow = true;
                live = Config::Get();
                draft = *live;
                editor.Reset();
                if (live->connection.displayName.empty())
                {
                    editor.FocusConnectionField();
                }
                stagedApplied = false;
                saveFailed = false;
            }

            void Apply(Config::PluginConfig a_config)
            {
                saveFailed = !Config::Apply(std::move(a_config));
                if (saveFailed)
                {
                    saveFailedText = Text::Format(Text::Id::kSettingsSaveFailed, kConfigFileName);
                }
                live = Config::Get();
            }

            void ReplaceStaged(const Config::DefaultOutfitSettings& a_outfit)
            {
                draft.defaultOutfit = a_outfit;
                editor.Reset();
            }

            void DrawWindow(bool& a_open)
            {
                const Theme::Scope theme;
                frame = MenuFrame::GetMetrics();
                const MenuFrame::WindowLayout layout{ .sizeFonts = kSizeFonts, .minimumSizeFonts = kMinimumSizeFonts };
                if (MenuFrame::BeginWindow(
                        "Multiplayer Settings###Settings", layout, kWindowFlags, std::exchange(focusWindow, false)))
                {
                    const auto fontSize = ImGui::GetFontSize();
                    const auto sidebarWidth = std::floor(kSidebarWidthFonts * fontSize);
                    DrawSidebarBackground(sidebarWidth);
                    // The page first: it finds the sections the sidebar lists.
                    ImGui::SetCursorPos(ImVec2{ frame.inset + sidebarWidth, frame.inset });
                    if (ImGui::BeginChild("##Page", ImVec2{ 0.0F, 0.0F }, ImGuiChildFlags_None, kFixedFlags))
                    {
                        DrawPage(a_open);
                    }
                    ImGui::EndChild();
                    ImGui::SetCursorPos(ImVec2{ frame.inset, frame.inset });
                    DrawSidebar(sidebarWidth);
                }
                ImGui::End();
            }

            void DrawSidebarBackground(float a_width) const
            {
                const auto position = ImGui::GetWindowPos();
                const auto left = position.x + frame.inset;
                const auto top = position.y + frame.inset;
                const auto bottom = position.y + ImGui::GetWindowHeight() - frame.inset;
                auto& list = *ImGui::GetWindowDrawList();
                list.AddRectFilled(ImVec2{ left, top }, ImVec2{ left + a_width, bottom },
                    ImGui::GetColorU32(Theme::Tint(0.03F)), (std::max)(frame.rounding - frame.inset, 0.0F),
                    ImDrawFlags_RoundCornersLeft);
                list.AddLine(ImVec2{ left + a_width, top }, ImVec2{ left + a_width, bottom },
                    ImGui::GetColorU32(ImGuiCol_Separator), ImGui::GetStyle().WindowBorderSize);
            }

            void DrawSidebar(float a_width)
            {
                const auto fontSize = ImGui::GetFontSize();
                const ImVec2 padding{ std::floor(kSidebarPaddingFonts.x * fontSize),
                    std::floor(kSidebarPaddingFonts.y * fontSize) };
                const auto inset = std::floor(kNavigationInsetFonts * fontSize);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding);
                const bool visible = ImGui::BeginChild(
                    "##Sidebar", ImVec2{ a_width, 0.0F }, ImGuiChildFlags_AlwaysUseWindowPadding, kFixedFlags);
                ImGui::PopStyleVar();
                if (visible)
                {
                    const auto& style = ImGui::GetStyle();
                    ImGui::SetCursorPosX(padding.x + inset);
                    DrawBrand(a_width - (padding.x + inset) * 2.0F);
                    ImGui::Dummy(ImVec2{ 0.0F, std::floor(style.ItemSpacing.y * 0.5F) });
                    editor.DrawNavigation();

                    ImGui::SetCursorPos(
                        ImVec2{ padding.x + inset, ImGui::GetWindowHeight() - padding.y - ImGui::GetTextLineHeight() });
                    ConnectionControls::DrawSummary(a_width - (padding.x + inset) * 2.0F);
                }
                ImGui::EndChild();
            }

            static void DrawBrand(float a_width)
            {
                const auto& style = ImGui::GetStyle();
                auto* logo = Fonts::GetLogo();
                if (!logo)
                {
                    ImGui::PushFont(Fonts::GetHeading(), style.FontSizeBase * kBrandScale);
                    ImGui::TextUnformatted(Text::GetCString(Text::Id::kSettingsBrand));
                    ImGui::PopFont();
                    return;
                }
                // The logo font's em is the logo's height. Dear ImGui rounds a font size
                // to whole pixels, so a whole height keeps the logo within the width.
                const auto aspect =
                    logo->CalcTextSizeA(style.FontSizeBase, FLT_MAX, 0.0F, kLogoLetters).x / style.FontSizeBase;
                ImGui::PushFont(logo, std::floor(a_width / aspect) / (style.FontScaleMain * style.FontScaleDpi));
                const auto position = ImGui::GetCursorScreenPos();
                auto& list = *ImGui::GetWindowDrawList();
                list.AddText(position, ImGui::GetColorU32(ImGuiCol_Text), kLogoLetters);
                list.AddText(position, ImGui::GetColorU32(Theme::GetAccent()), kLogoBar);
                ImGui::Dummy(ImGui::CalcTextSize(kLogoLetters));
                ImGui::PopFont();
            }

            void DrawPage(bool& a_open)
            {
                const auto fontSize = ImGui::GetFontSize();
                const ImVec2 padding{ std::floor(kPagePaddingFonts.x * fontSize),
                    std::floor(kPagePaddingFonts.y * fontSize) };
                const auto footerGap = std::floor(kFooterGapFonts * fontSize);
                const auto footerHeight = ImGui::GetFrameHeight() + footerGap * 2.0F;
                const auto* liveError = GetLiveError();
                const auto knot = frame.corner - frame.inset;
                editor.DrawPage(draft, padding, footerHeight, liveError ? footerHeight : (std::max)(padding.y, knot));
                ApplyLiveSections();
                // After applying: the hello reads the display name from the live configuration.
                if (editor.TakeConnectRequest())
                {
                    ConnectionControls::Connect();
                }

                const bool staged = editor.IsStagedSectionOpen();
                const auto defaultsSection = editor.GetDefaultsSection();
                if (staged || !defaultsSection.empty() || liveError)
                {
                    auto top = ImGui::GetWindowHeight() - footerHeight;
                    const auto position = ImGui::GetWindowPos();
                    ImGui::GetWindowDrawList()->AddLine(ImVec2{ position.x, position.y + top },
                        ImVec2{ position.x + ImGui::GetWindowWidth(), position.y + top },
                        ImGui::GetColorU32(ImGuiCol_Separator), ImGui::GetStyle().WindowBorderSize);
                    ImGui::SetCursorPos(ImVec2{ padding.x, top + footerGap });
                    if (staged)
                    {
                        DrawStagedFooter(padding.x + editor.GetScrollbarWidth());
                    }
                    else
                    {
                        top = ImGui::GetCursorPosY();
                        if (liveError)
                        {
                            ImGui::AlignTextToFramePadding();
                            ImGui::TextColored(Theme::kNegative, "%s", liveError);
                        }
                        if (!defaultsSection.empty())
                        {
                            DrawDefaultsFooter(defaultsSection, padding.x + editor.GetScrollbarWidth(), top);
                        }
                    }
                }

                const auto inset = std::floor(kCloseInsetFonts * fontSize);
                ImGui::SetCursorPos(
                    ImVec2{ ImGui::GetWindowWidth() - (std::max)(inset, knot) - ImGui::GetFrameHeight(), inset });
                if (Controls::IconButton("##Close", Controls::Icon::kClose))
                {
                    a_open = false;
                }
            }

            void ApplyLiveSections()
            {
                if (editor.IsEditing() || LiveSectionsMatch(draft, *live))
                {
                    return;
                }
                auto config = draft;
                config.defaultOutfit = live->defaultOutfit;
                Apply(std::move(config));
            }

            // One width for every footer button, so the staged footer's three line up.
            static ImVec2 GetFooterButtonSize()
            {
                float width = std::floor(kFooterButtonWidthFonts * ImGui::GetFontSize());
                for (const auto id : { Text::Id::kSettingsDefaults, Text::Id::kSettingsRevert, Text::Id::kSettingsApply })
                {
                    width = (std::max)(width,
                        std::ceil(ImGui::CalcTextSize(Text::GetCString(id)).x + ImGui::GetStyle().FramePadding.x * 2.0F));
                }
                return ImVec2{ width, 0.0F };
            }

            void DrawDefaultsFooter(std::string_view a_section, float a_inset, float a_top)
            {
                const auto size = GetFooterButtonSize();
                ImGui::SetCursorPos(ImVec2{ ImGui::GetWindowWidth() - a_inset - size.x, a_top });
                ImGui::BeginDisabled(HasDefaults(a_section));
                if (ImGui::Button(Text::GetCString(Text::Id::kSettingsDefaults), size))
                {
                    RestoreDefaults(a_section);
                    ApplyLiveSections();
                }
                ImGui::EndDisabled();
            }

            // The chat's size is set by dragging the chat, not on the page, so it is kept.
            [[nodiscard]] Config::UISettings GetShownUIDefaults() const
            {
                auto ui = defaults.ui;
                ui.chatWidth = draft.ui.chatWidth;
                ui.chatLines = draft.ui.chatLines;
                return ui;
            }

            [[nodiscard]] bool HasDefaults(std::string_view a_section) const
            {
                if (a_section == kUISection)
                {
                    return draft.ui == GetShownUIDefaults();
                }
                if (a_section == kControlsSection)
                {
                    return draft.controls == defaults.controls;
                }
                return draft.advanced == defaults.advanced;
            }

            void RestoreDefaults(std::string_view a_section)
            {
                if (a_section == kUISection)
                {
                    draft.ui = GetShownUIDefaults();
                }
                else if (a_section == kControlsSection)
                {
                    draft.controls = defaults.controls;
                }
                else
                {
                    draft.advanced = defaults.advanced;
                }
            }

            void DrawStagedFooter(float a_inset)
            {
                const bool changed = draft.defaultOutfit != live->defaultOutfit;
                if (changed)
                {
                    stagedApplied = false;
                }

                const auto top = ImGui::GetCursorPosY();
                ImGui::AlignTextToFramePadding();
                if (changed)
                {
                    ImGui::TextDisabled("%s", Text::GetCString(Text::Id::kSettingsUnapplied));
                }
                else if (saveFailed)
                {
                    ImGui::TextColored(Theme::kNegative, "%s", saveFailedText.c_str());
                }
                else if (stagedApplied)
                {
                    ImGui::TextColored(Theme::kPositive, "%s", Text::GetCString(Text::Id::kSettingsSaved));
                }

                const auto size = GetFooterButtonSize();
                ImGui::SetCursorPos(ImVec2{
                    ImGui::GetWindowWidth() - a_inset - size.x * 3.0F - ImGui::GetStyle().ItemSpacing.x * 2.0F, top });
                ImGui::BeginDisabled(draft.defaultOutfit == defaults.defaultOutfit);
                if (ImGui::Button(Text::GetCString(Text::Id::kSettingsDefaults), size))
                {
                    ReplaceStaged(defaults.defaultOutfit);
                }
                ImGui::EndDisabled();

                ImGui::SameLine();
                ImGui::BeginDisabled(!changed);
                if (ImGui::Button(Text::GetCString(Text::Id::kSettingsRevert), size))
                {
                    ReplaceStaged(live->defaultOutfit);
                }
                ImGui::SameLine();
                const auto* apply = Text::GetCString(Text::Id::kSettingsApply);
                if (changed ? Controls::PrimaryButton(apply, size) : ImGui::Button(apply, size))
                {
                    auto config = *live;
                    config.defaultOutfit = draft.defaultOutfit;
                    Apply(std::move(config));
                    stagedApplied = true;
                }
                ImGui::EndDisabled();
            }

            [[nodiscard]] const char* GetLiveError() const { return saveFailed ? saveFailedText.c_str() : nullptr; }

            const Config::PluginConfig defaults;
            std::shared_ptr<const Config::PluginConfig> live;
            Config::PluginConfig draft;
            SettingsEditor editor;
            MenuFrame::Metrics frame;
            std::string saveFailedText;
            bool stagedApplied{ false };
            bool saveFailed{ false };
            bool wasFocused{ false };
            bool focusWindow{ false };
        };
    }

    std::unique_ptr<Window> CreateSettingsWindow()
    {
        return std::make_unique<SettingsWindow>();
    }
}
