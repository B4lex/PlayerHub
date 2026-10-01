#include "ui/settings/SettingsWindow.h"
#include "config/PluginConfig.h"
#include "config/SettingsSchema.h"
#include "ui/Theme.h"
#include "ui/WindowManager.h"
#include "ui/settings/ConnectionControls.h"
#include "ui/widgets/Controls.h"
#include "ui/widgets/ItemPicker.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <memory>
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
        // In font sizes, so the layout follows the overlay scale.
        constexpr ImVec2 kSizeFonts{ 48.0F, 32.0F };
        constexpr ImVec2 kMinimumSizeFonts{ 40.0F, 20.0F };
        constexpr float kSidebarWidthFonts = 11.5F;
        constexpr ImVec2 kSidebarPaddingFonts{ 0.5F, 1.25F };
        constexpr float kNavigationHeightFonts = 2.125F;
        constexpr float kNavigationInsetFonts = 0.875F;
        constexpr float kNavigationGapFonts = 0.125F;
        constexpr float kNavigationMarkFonts = 0.1875F;
        constexpr float kNavigationIdleAlpha = 0.68F;
        constexpr ImVec2 kPagePaddingFonts{ 1.5F, 1.125F };
        constexpr float kCloseInsetFonts = 0.75F;
        constexpr float kFooterGapFonts = 0.75F;
        constexpr float kFooterButtonWidthFonts = 5.5F;
        constexpr float kTooltipWidthFonts = 22.0F;
        // Of the base font size.
        constexpr float kTitleScale = 1.375F;
        constexpr float kBrandScale = 1.125F;
        constexpr float kGroupScale = 0.8125F;
        // Of a row's width; its label has the rest.
        constexpr float kControlWidth = 0.58F;
        // Steps from zero to a number setting's maximum.
        constexpr std::uint32_t kNumberSteps = 100;

        // The section and its setting that are drawn as a field beside the
        // connection button rather than as a row.
        constexpr std::string_view kConnectionSection = "General";
        constexpr std::string_view kDisplayNameKey = "DisplayName";
        constexpr const char* kDisplayNameHint = "Enter a display name";
        // The section whose edits wait for Apply. The others take effect as they
        // are edited.
        constexpr std::string_view kStagedSection = "DefaultOutfit";
        // Sections whose file name is not what a player would call them.
        constexpr std::pair<std::string_view, std::string_view> kSectionTitles[]{ { "UI", "Interface" } };

        constexpr float kBackgroundAlpha = 0.96F;
        constexpr const char* kSaveFailedText = "Could not save skyrim-mp.ini";

        // Only a section's settings scroll, so the navigation, the title, and the
        // buttons stay in view.
        constexpr ImGuiWindowFlags kFixedFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse
            | ImGuiWindowFlags_NoTitleBar | kFixedFlags;

        // "SpawnsPerSecond" becomes "Spawns Per Second". Reuses a_result's storage.
        void FormatDisplayName(std::string_view a_name, std::string& a_result)
        {
            a_result.clear();
            for (std::size_t index = 0; index < a_name.size(); ++index)
            {
                const auto character = static_cast<unsigned char>(a_name[index]);
                if (index > 0 && std::isupper(character) && std::islower(static_cast<unsigned char>(a_name[index - 1])))
                {
                    a_result += ' ';
                }
                a_result += a_name[index];
            }
        }

        void FormatSectionTitle(std::string_view a_name, std::string& a_result)
        {
            for (const auto& [name, title] : kSectionTitles)
            {
                if (name == a_name)
                {
                    a_result.assign(title);
                    return;
                }
            }
            FormatDisplayName(a_name, a_result);
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

        bool DrawNavigationItem(const std::string& a_label, bool a_selected)
        {
            const auto fontSize = ImGui::GetFontSize();
            const auto position = ImGui::GetCursorScreenPos();
            const ImVec2 size{ ImGui::GetContentRegionAvail().x, std::floor(kNavigationHeightFonts * fontSize) };
            const bool pressed = ImGui::InvisibleButton(a_label.c_str(), size);
            const bool hovered = ImGui::IsItemHovered();

            auto& list = *ImGui::GetWindowDrawList();
            const ImVec2 end{ position.x + size.x, position.y + size.y };
            if (a_selected || hovered)
            {
                list.AddRectFilled(position, end,
                    ImGui::GetColorU32(a_selected ? Theme::Accent(0.16F) : Theme::Tint(0.06F)),
                    ImGui::GetStyle().FrameRounding);
            }
            if (a_selected)
            {
                const auto width = std::floor(kNavigationMarkFonts * fontSize);
                const auto inset = std::floor(size.y * 0.28F);
                list.AddRectFilled(ImVec2{ position.x, position.y + inset },
                    ImVec2{ position.x + width, end.y - inset }, ImGui::GetColorU32(Theme::kAccent), width * 0.5F);
            }
            list.AddText(ImVec2{ position.x + std::floor(kNavigationInsetFonts * fontSize),
                             position.y + std::floor((size.y - fontSize) * 0.5F) },
                ImGui::GetColorU32(ImGuiCol_Text, a_selected || hovered ? 1.0F : kNavigationIdleAlpha),
                a_label.c_str());
            return pressed;
        }

        // Draws the settings of one section of a configuration at a time, each
        // as a widget that edits it in place, and the list that picks the
        // section. A text field sets its value when editing it ends; text that
        // does not parse stays in the field and leaves the setting at its last
        // valid value.
        class SettingsEditor final : public Config::SettingsVisitor
        {
        public:
            // Fields show the configuration's values again on the next draw.
            void Reset()
            {
                fields.clear();
                itemPicker.Reset();
            }

            // Fields whose text is not applied, other than one that says so itself.
            [[nodiscard]] bool HasInvalidFields() const
            {
                return std::ranges::any_of(
                    fields, [](const auto& a_field) { return !a_field.second.valid && !a_field.second.reportsItself; });
            }

            // Shows the connection section with the keyboard in its field, on the
            // next DrawPage.
            void FocusConnectionField()
            {
                openSection = kConnectionSection;
                focusConnectionField = true;
            }

            // As of the last DrawPage.
            [[nodiscard]] bool IsStagedSectionOpen() const { return stagedSectionOpen; }

            // A number is being typed or stepped, as of the last DrawPage.
            [[nodiscard]] bool IsEditing() const { return editing; }

            // What the open section's scrollbar takes from its settings' width, as
            // of the last DrawPage.
            [[nodiscard]] float GetScrollbarWidth() const { return scrollbarWidth; }

            // The selected section's title, then its settings a_padding.x in from
            // both sides. Each height is left free below the settings of a
            // section of that kind.
            void DrawPage(Config::PluginConfig& a_config, const ImVec2& a_padding, float a_stagedFooterHeight,
                float a_liveFooterHeight)
            {
                padding = a_padding;
                stagedFooterHeight = a_stagedFooterHeight;
                liveFooterHeight = a_liveFooterHeight;
                stagedSectionOpen = false;
                editing = false;
                sections.clear();
                Config::Describe(a_config, *this);
                EndSection();
            }

            // One item per section DrawPage went through. A pick shows on the next
            // DrawPage, so a field edited until the press still sees its editing end.
            void DrawNavigation()
            {
                ImGui::PushStyleVar(
                    ImGuiStyleVar_ItemSpacing, ImVec2{ 0.0F, std::floor(kNavigationGapFonts * ImGui::GetFontSize()) });
                for (const auto name : sections)
                {
                    FormatSectionTitle(name, label);
                    if (DrawNavigationItem(label, name == openSection))
                    {
                        openSection = name;
                    }
                }
                ImGui::PopStyleVar();
            }

            void Section(std::string_view a_name, std::string_view a_comment) override
            {
                EndSection();
                sections.push_back(a_name);
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
                ImGui::PushID(a_name.data(), a_name.data() + a_name.size());

                const auto& style = ImGui::GetStyle();
                ImGui::SetCursorPos(padding);
                FormatSectionTitle(a_name, label);
                ImGui::PushFont(nullptr, style.FontSizeBase * kTitleScale);
                DrawText(label);
                ImGui::PopFont();
                if (!a_comment.empty())
                {
                    ImGui::SetCursorPosX(padding.x);
                    FormatNote(a_comment);
                    ImGui::PushTextWrapPos(ImGui::GetWindowWidth() - padding.x);
                    DrawMutedText(note);
                    ImGui::PopTextWrapPos();
                }
                ImGui::Dummy(ImVec2{ 0.0F, std::floor(style.ItemSpacing.y * 0.5F) });

                // The scrollbar runs along the window's edge, outside the padding.
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ padding.x, 0.0F });
                ImGui::BeginChild("##Content",
                    ImVec2{ 0.0F, -(stagedSectionOpen ? stagedFooterHeight : liveFooterHeight) },
                    ImGuiChildFlags_AlwaysUseWindowPadding);
                ImGui::PopStyleVar();
            }

            void Comment(std::string_view a_comment) override
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
                for (const auto character : a_comment)
                {
                    note += static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
                }
                ImGui::PushFont(nullptr, style.FontSizeBase * kGroupScale);
                DrawMutedText(note);
                ImGui::PopFont();
            }

            void Bool(std::string_view a_key, bool& a_value, std::string_view a_comment) override
            {
                if (!BeginRow(a_key, a_comment))
                {
                    return;
                }
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + controlWidth - Controls::GetToggleWidth());
                Controls::Toggle("##value", a_value);
                EndRow();
            }

            void Unsigned(std::string_view a_key, std::uint32_t& a_value, std::string_view a_comment,
                std::uint32_t a_maximum, std::string_view a_zeroName) override
            {
                if (!BeginRow(a_key, a_comment))
                {
                    return;
                }
                // A named zero shows as an empty field under its name.
                const auto flags = a_zeroName.empty()
                    ? ImGuiInputTextFlags_None
                    : ImGuiInputTextFlags_DisplayEmptyRefVal | ImGuiInputTextFlags_ParseEmptyRefVal;
                const std::uint32_t step = (std::max)(a_maximum / kNumberSteps, 1U);
                if (ImGui::InputScalar("##value", ImGuiDataType_U32, &a_value, &step, nullptr, nullptr, flags))
                {
                    a_value = (std::min)(a_value, a_maximum);
                }
                const bool active = ImGui::IsItemActive();
                editing = editing || active;
                if (a_value == 0 && !active && !a_zeroName.empty())
                {
                    const auto position = ImGui::GetItemRectMin();
                    const auto& framePadding = ImGui::GetStyle().FramePadding;
                    ImGui::GetWindowDrawList()->AddText(
                        ImVec2{ position.x + framePadding.x, position.y + framePadding.y },
                        ImGui::GetColorU32(ImGuiCol_TextDisabled), a_zeroName.data(),
                        a_zeroName.data() + a_zeroName.size());
                }
                EndRow();
            }

            void String(std::string_view a_key, std::string& a_value, std::string_view a_comment,
                bool (*a_isValid)(std::string_view)) override
            {
                if (sectionOpen && openSection == kConnectionSection && a_key == kDisplayNameKey)
                {
                    DrawConnectionBar(a_key, a_value, a_comment, a_isValid);
                    return;
                }
                if (!BeginRow(a_key, a_comment))
                {
                    return;
                }
                auto& field = GetField([&a_value] { return a_value; });
                if (DrawTextField(field, nullptr))
                {
                    field.valid = a_isValid(field.text);
                }
                if (ImGui::IsItemDeactivatedAfterEdit() && field.valid)
                {
                    a_value = field.text;
                }
                if (!field.valid)
                {
                    DrawTooltip(a_comment);
                }
                EndRow();
            }

            void Form(std::string_view a_key, Multiplayer::FormReference& a_value,
                const Multiplayer::FormReference& a_default, const Forms::ItemFilter& a_filter) override
            {
                if (!BeginRow(a_key, {}))
                {
                    return;
                }
                itemPicker.Draw("##value", a_value, a_default, a_filter);
                EndRow();
            }

            void Choice(std::string_view a_key, std::uint32_t& a_value, std::string_view a_comment,
                std::span<const std::string_view> a_options) override
            {
                if (!BeginRow(a_key, a_comment))
                {
                    return;
                }
                label.assign(Config::FormatChoice(a_value, a_options));
                if (ImGui::BeginCombo("##value", label.c_str()))
                {
                    for (std::uint32_t index = 0; index < a_options.size(); ++index)
                    {
                        label.assign(a_options[index]);
                        const bool selected = index == a_value;
                        if (ImGui::Selectable(label.c_str(), selected))
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

            void Key(std::string_view a_key, std::uint32_t& a_value, std::string_view a_comment) override
            {
                if (!BeginRow(a_key, a_comment))
                {
                    return;
                }
                const auto current = Config::FormatKey(a_value);
                label.assign(current);
                if (ImGui::BeginCombo("##value", label.c_str()))
                {
                    for (const auto& key : Config::GetBindableKeys())
                    {
                        label.assign(key.name);
                        const bool selected = key.scanCode == a_value;
                        if (ImGui::Selectable(label.c_str(), selected))
                        {
                            a_value = key.scanCode;
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

        private:
            struct TextField
            {
                std::string text;
                bool valid{ true };
                // Its own line says why the text is not applied.
                bool reportsItself{ false };
            };

            // The setting's name over one line of its field and the connection
            // button, so the name and joining with it read as one step, then
            // ConnectionControls' line on either. The field is locked, with the
            // reason as its tooltip, unless disconnected.
            void DrawConnectionBar(std::string_view a_key, std::string& a_value, std::string_view a_comment,
                bool (*a_isValid)(std::string_view))
            {
                sectionEmpty = false;
                ImGui::PushID(a_key.data(), a_key.data() + a_key.size());
                FormatDisplayName(a_key, label);
                DrawText(label);

                auto& field = GetField([&a_value] { return a_value; });
                field.reportsItself = true;
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - ConnectionControls::GetButtonWidth()
                    - ImGui::GetStyle().ItemSpacing.x);
                if (focusConnectionField)
                {
                    ImGui::SetKeyboardFocusHere();
                    focusConnectionField = false;
                }
                const auto* lock = ConnectionControls::GetDisplayNameLock();
                ImGui::BeginDisabled(lock != nullptr);
                if (DrawTextField(field, kDisplayNameHint))
                {
                    field.valid = a_isValid(field.text);
                }
                ImGui::EndDisabled();
                const bool entered = field.valid && !field.text.empty();
                if (ImGui::IsItemDeactivatedAfterEdit() && field.valid)
                {
                    a_value = field.text;
                }
                if (lock)
                {
                    DrawTooltip(lock, ImGuiHoveredFlags_AllowWhenDisabled);
                }

                ImGui::SameLine();
                ConnectionControls::DrawButton(entered);
                FormatNote(a_comment);
                ConnectionControls::DrawNotice(!field.valid, note);
                ImGui::PopID();
            }

            void EndSection()
            {
                if (!sectionOpen)
                {
                    return;
                }
                // Keeps the last row off the edge when the section scrolls.
                ImGui::Dummy(ImVec2{ 0.0F, std::floor(ImGui::GetStyle().ItemSpacing.y * 0.5F) });
                scrollbarWidth = ImGui::GetScrollMaxY() > 0.0F ? ImGui::GetStyle().ScrollbarSize : 0.0F;
                ImGui::EndChild();
                ImGui::PopID();
                sectionOpen = false;
            }

            // The label, with a_comment as its tooltip, then the cursor at the
            // control's place.
            bool BeginRow(std::string_view a_key, std::string_view a_comment)
            {
                if (!sectionOpen)
                {
                    return false;
                }
                sectionEmpty = false;
                ImGui::PushID(a_key.data(), a_key.data() + a_key.size());
                const auto left = ImGui::GetCursorPosX();
                const auto width = ImGui::GetContentRegionAvail().x;
                controlWidth = std::floor(width * kControlWidth);

                FormatDisplayName(a_key, label);
                ImGui::AlignTextToFramePadding();
                DrawText(label);
                DrawTooltip(a_comment);
                ImGui::SameLine(left + width - controlWidth);
                ImGui::SetNextItemWidth(controlWidth);
                return true;
            }

            void EndRow() { ImGui::PopID(); }

            // Comments are wrapped for the file; the window wraps them itself.
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

            // Returns whether the text changed. a_hint, if any, shows in an empty
            // field.
            static bool DrawTextField(TextField& a_field, const char* a_hint)
            {
                if (!a_field.valid)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, Theme::kNegative);
                }
                const bool changed = a_hint ? ImGui::InputTextWithHint("##value", a_hint, &a_field.text)
                                            : ImGui::InputText("##value", &a_field.text);
                if (!a_field.valid)
                {
                    ImGui::PopStyleColor();
                }
                return changed;
            }

            std::unordered_map<ImGuiID, TextField> fields;
            ItemPicker itemPicker;
            // Reused so drawing formats labels and notes without allocating.
            std::string label;
            std::string note;
            // Describe passes literals, so the views outlive the pass.
            std::vector<std::string_view> sections;
            std::string_view openSection;
            ImVec2 padding;
            float stagedFooterHeight{ 0.0F };
            float liveFooterHeight{ 0.0F };
            float controlWidth{ 0.0F };
            float scrollbarWidth{ 0.0F };
            bool sectionOpen{ false };
            // Nothing was drawn in the open section yet.
            bool sectionEmpty{ false };
            bool stagedSectionOpen{ false };
            bool editing{ false };
            bool focusConnectionField{ false };
        };

        bool LiveSectionsMatch(const Config::PluginConfig& a_left, const Config::PluginConfig& a_right)
        {
            return a_left.general == a_right.general && a_left.network == a_right.network
                && a_left.performance == a_right.performance && a_left.ui == a_right.ui;
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
                return a_scanCode == Config::Get()->ui.settingsKey;
            }

            [[nodiscard]] bool IsReleaseKey(std::uint32_t a_scanCode) const override { return IsFocusKey(a_scanCode); }

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
                if (live->general.displayName.empty())
                {
                    editor.FocusConnectionField();
                }
                stagedApplied = false;
                saveFailed = false;
            }

            void Apply(Config::PluginConfig a_config)
            {
                saveFailed = !Config::Apply(std::move(a_config));
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
                const auto& display = ImGui::GetIO().DisplaySize;
                const auto fontSize = ImGui::GetFontSize();
                ImGui::SetNextWindowPos(
                    ImVec2{ display.x * 0.5F, display.y * 0.5F }, ImGuiCond_Appearing, ImVec2{ 0.5F, 0.5F });
                ImGui::SetNextWindowSize(
                    ImVec2{ kSizeFonts.x * fontSize, kSizeFonts.y * fontSize }, ImGuiCond_Appearing);
                ImGui::SetNextWindowSizeConstraints(
                    ImVec2{ kMinimumSizeFonts.x * fontSize, kMinimumSizeFonts.y * fontSize }, display);
                ImGui::SetNextWindowBgAlpha(kBackgroundAlpha);
                if (focusWindow)
                {
                    ImGui::SetNextWindowFocus();
                    focusWindow = false;
                }
                // The sidebar and the page reach the window's edges and pad themselves.
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ 0.0F, 0.0F });
                const bool visible = ImGui::Begin("Multiplayer Settings###Settings", nullptr, kWindowFlags);
                ImGui::PopStyleVar();
                if (visible)
                {
                    const auto sidebarWidth = std::floor(kSidebarWidthFonts * fontSize);
                    DrawSidebarBackground(sidebarWidth);
                    // The page first: it finds the sections the sidebar lists.
                    ImGui::SetCursorPos(ImVec2{ sidebarWidth, 0.0F });
                    if (ImGui::BeginChild("##Page", ImVec2{ 0.0F, 0.0F }, ImGuiChildFlags_None, kFixedFlags))
                    {
                        DrawPage(a_open);
                    }
                    ImGui::EndChild();
                    ImGui::SetCursorPos(ImVec2{ 0.0F, 0.0F });
                    DrawSidebar(sidebarWidth);
                }
                ImGui::End();
            }

            // A faint panel over the window's left side, inside its border.
            static void DrawSidebarBackground(float a_width)
            {
                const auto& style = ImGui::GetStyle();
                const auto border = style.WindowBorderSize;
                const auto position = ImGui::GetWindowPos();
                const auto bottom = position.y + ImGui::GetWindowHeight() - border;
                auto& list = *ImGui::GetWindowDrawList();
                list.AddRectFilled(ImVec2{ position.x + border, position.y + border },
                    ImVec2{ position.x + a_width, bottom }, ImGui::GetColorU32(Theme::Tint(0.03F)),
                    style.WindowRounding - border, ImDrawFlags_RoundCornersLeft);
                list.AddLine(ImVec2{ position.x + a_width, position.y + border },
                    ImVec2{ position.x + a_width, bottom }, ImGui::GetColorU32(ImGuiCol_Separator), border);
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
                    ImGui::PushFont(nullptr, style.FontSizeBase * kBrandScale);
                    ImGui::TextUnformatted("Multiplayer");
                    ImGui::PopFont();
                    ImGui::Dummy(ImVec2{ 0.0F, std::floor(style.ItemSpacing.y * 0.5F) });
                    editor.DrawNavigation();

                    ImGui::SetCursorPos(
                        ImVec2{ padding.x + inset, ImGui::GetWindowHeight() - padding.y - ImGui::GetTextLineHeight() });
                    ConnectionControls::DrawSummary(a_width - (padding.x + inset) * 2.0F);
                }
                ImGui::EndChild();
            }

            void DrawPage(bool& a_open)
            {
                const auto fontSize = ImGui::GetFontSize();
                const ImVec2 padding{ std::floor(kPagePaddingFonts.x * fontSize),
                    std::floor(kPagePaddingFonts.y * fontSize) };
                // Below a line under a section's settings, which only some sections have.
                const auto footerGap = std::floor(kFooterGapFonts * fontSize);
                const auto footerHeight = ImGui::GetFrameHeight() + footerGap * 2.0F;
                const auto* liveError = GetLiveError();
                editor.DrawPage(draft, padding, footerHeight, liveError ? footerHeight : padding.y);
                ApplyLiveSections();

                const bool staged = editor.IsStagedSectionOpen();
                if (staged || liveError)
                {
                    const auto top = ImGui::GetWindowHeight() - footerHeight;
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
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextColored(Theme::kNegative, "%s", liveError);
                    }
                }

                const auto inset = std::floor(kCloseInsetFonts * fontSize);
                ImGui::SetCursorPos(ImVec2{ ImGui::GetWindowWidth() - inset - ImGui::GetFrameHeight(), inset });
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

            // At the cursor, what became of the edits; the buttons end a_inset from
            // the page's right edge.
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
                    ImGui::TextDisabled("Unapplied changes");
                }
                else if (saveFailed)
                {
                    ImGui::TextColored(Theme::kNegative, "%s", kSaveFailedText);
                }
                else if (stagedApplied)
                {
                    ImGui::TextColored(Theme::kPositive, "Saved");
                }

                const ImVec2 size{ std::floor(kFooterButtonWidthFonts * ImGui::GetFontSize()), 0.0F };
                ImGui::SetCursorPos(ImVec2{
                    ImGui::GetWindowWidth() - a_inset - size.x * 3.0F - ImGui::GetStyle().ItemSpacing.x * 2.0F, top });
                ImGui::BeginDisabled(draft.defaultOutfit == defaults.defaultOutfit);
                if (ImGui::Button("Defaults", size))
                {
                    ReplaceStaged(defaults.defaultOutfit);
                }
                ImGui::EndDisabled();

                ImGui::SameLine();
                ImGui::BeginDisabled(!changed);
                if (ImGui::Button("Revert", size))
                {
                    ReplaceStaged(live->defaultOutfit);
                }
                ImGui::SameLine();
                if (changed ? Controls::PrimaryButton("Apply", size) : ImGui::Button("Apply", size))
                {
                    auto config = *live;
                    config.defaultOutfit = draft.defaultOutfit;
                    Apply(std::move(config));
                    stagedApplied = true;
                }
                ImGui::EndDisabled();
            }

            // What a section without buttons has to report, if anything.
            [[nodiscard]] const char* GetLiveError() const
            {
                if (editor.HasInvalidFields())
                {
                    return "Values in red are not applied";
                }
                return saveFailed ? kSaveFailedText : nullptr;
            }

            const Config::PluginConfig defaults;
            std::shared_ptr<const Config::PluginConfig> live;
            Config::PluginConfig draft;
            SettingsEditor editor;
            // The staged section was applied and has not been edited since.
            bool stagedApplied{ false };
            // The latest Apply could not write the file.
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
