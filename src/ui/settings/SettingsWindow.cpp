#include "ui/settings/SettingsWindow.h"
#include "config/PluginConfig.h"
#include "config/SettingsSchema.h"
#include "ui/WindowManager.h"
#include "ui/widgets/ItemPicker.h"

#include <imgui.h>
#include <misc/cpp/imgui_stdlib.h>

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

namespace UI
{
    namespace
    {
        // In font sizes, so the layout follows the overlay scale.
        constexpr ImVec2 kSizeFonts{ 44.0F, 32.0F };
        constexpr float kLabelWidthFonts = 11.0F;

        constexpr float kBackgroundAlpha = 0.94F;
        constexpr ImVec4 kNoteColor{ 0.70F, 0.70F, 0.70F, 1.0F };
        constexpr ImVec4 kErrorColor{ 0.95F, 0.40F, 0.35F, 1.0F };
        constexpr ImVec4 kSuccessColor{ 0.45F, 0.85F, 0.45F, 1.0F };

        // Only each tab's content scrolls, so the tabs and buttons stay in view.
        constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoCollapse
            | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;

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

        // Comments are wrapped for the file; the window wraps them itself.
        void DrawNote(std::string_view a_text, std::string& a_buffer)
        {
            if (a_text.empty())
            {
                return;
            }
            a_buffer.assign(a_text);
            std::ranges::replace(a_buffer, '\n', ' ');
            ImGui::PushStyleColor(ImGuiCol_Text, kNoteColor);
            ImGui::PushTextWrapPos(0.0F);
            ImGui::TextUnformatted(a_buffer.c_str(), a_buffer.c_str() + a_buffer.size());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }

        // Draws every setting of a configuration as a widget that edits it in
        // place, one tab per section. Text that does not parse stays in its field
        // and leaves the setting at its last valid value.
        class SettingsEditor final : public Config::SettingsVisitor
        {
        public:
            // Fields show the configuration's values again on the next draw.
            void Reset()
            {
                fields.clear();
                itemPicker.Reset();
            }

            [[nodiscard]] bool HasInvalidFields() const
            {
                return std::ranges::any_of(fields, [](const auto& a_field) { return !a_field.second.valid; });
            }

            void Draw(Config::PluginConfig& a_config, float a_footerHeight)
            {
                footerHeight = a_footerHeight;
                if (ImGui::BeginTabBar("##Sections"))
                {
                    Config::Describe(a_config, *this);
                    EndSection();
                    ImGui::EndTabBar();
                }
            }

            void Section(std::string_view a_name, std::string_view a_comment) override
            {
                EndSection();
                FormatDisplayName(a_name, label);
                ImGui::PushID(a_name.data(), a_name.data() + a_name.size());
                if (!ImGui::BeginTabItem(label.c_str()))
                {
                    ImGui::PopID();
                    return;
                }
                sectionOpen = true;
                ImGui::BeginChild("##Content", ImVec2{ 0.0F, -footerHeight });
                DrawNote(a_comment, note);
                ImGui::Spacing();
            }

            void Comment(std::string_view a_comment) override
            {
                if (!sectionOpen)
                {
                    return;
                }
                ImGui::Spacing();
                ImGui::Separator();
                DrawNote(a_comment, note);
            }

            void Bool(std::string_view a_key, bool& a_value, std::string_view a_comment) override
            {
                if (!BeginRow(a_key, true))
                {
                    return;
                }
                ImGui::Checkbox("##value", &a_value);
                EndRow(a_comment);
            }

            void Unsigned(std::string_view a_key, std::uint32_t& a_value, std::string_view a_comment,
                std::uint32_t a_maximum) override
            {
                if (!BeginRow(a_key, true))
                {
                    return;
                }
                constexpr std::uint32_t kStep = 1;
                if (ImGui::InputScalar("##value", ImGuiDataType_U32, &a_value, &kStep))
                {
                    a_value = (std::min)(a_value, a_maximum);
                }
                EndRow(a_comment);
            }

            void String(std::string_view a_key, std::string& a_value, std::string_view a_comment,
                bool (*a_isValid)(std::string_view)) override
            {
                if (!BeginRow(a_key, true))
                {
                    return;
                }
                auto& field = GetField([&a_value] { return a_value; });
                if (DrawTextField(field))
                {
                    field.valid = a_isValid(field.text);
                    if (field.valid)
                    {
                        a_value = field.text;
                    }
                }
                EndRow(a_comment);
            }

            void Form(
                std::string_view a_key, Multiplayer::FormReference& a_value, const Forms::ItemFilter& a_filter) override
            {
                if (!BeginRow(a_key, false))
                {
                    return;
                }
                itemPicker.Draw("##value", a_value, a_filter);
                EndRow({});
            }

            void Key(std::string_view a_key, std::uint32_t& a_value, std::string_view a_comment) override
            {
                if (!BeginRow(a_key, true))
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
                EndRow(a_comment);
            }

        private:
            struct TextField
            {
                std::string text;
                bool valid{ true };
            };

            void EndSection()
            {
                if (!sectionOpen)
                {
                    return;
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
                ImGui::PopID();
                sectionOpen = false;
            }

            // Form keys are shown as the file names them, since the group comment
            // explains them in those terms.
            bool BeginRow(std::string_view a_key, bool a_displayName)
            {
                if (!sectionOpen)
                {
                    return false;
                }
                ImGui::PushID(a_key.data(), a_key.data() + a_key.size());
                if (a_displayName)
                {
                    FormatDisplayName(a_key, label);
                }
                else
                {
                    label.assign(a_key);
                }
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label.c_str(), label.c_str() + label.size());
                ImGui::SameLine(kLabelWidthFonts * ImGui::GetFontSize());
                ImGui::SetNextItemWidth(-FLT_MIN);
                return true;
            }

            void EndRow(std::string_view a_comment)
            {
                DrawNote(a_comment, note);
                ImGui::PopID();
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

            // Returns whether the text changed.
            static bool DrawTextField(TextField& a_field)
            {
                if (!a_field.valid)
                {
                    ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
                }
                const bool changed = ImGui::InputText("##value", &a_field.text);
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
            float footerHeight{ 0.0F };
            bool sectionOpen{ false };
        };

        class SettingsWindow final : public Window
        {
        public:
            SettingsWindow()
                : defaults(Config::MakeDefaultConfig())
            {
            }

            [[nodiscard]] bool IsFocusKey(std::uint32_t a_scanCode) const override
            {
                return a_scanCode == Config::Get()->ui.settingsKey;
            }

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
            enum class Status
            {
                kNone,
                kSaved,
                kNotSaved
            };

            void Open()
            {
                wasFocused = true;
                focusWindow = true;
                live = Config::Get();
                draft = *live;
                editor.Reset();
                status = Status::kNone;
            }

            void Replace(const Config::PluginConfig& a_config)
            {
                draft = a_config;
                editor.Reset();
            }

            void DrawWindow(bool& a_open)
            {
                const auto& display = ImGui::GetIO().DisplaySize;
                const auto fontSize = ImGui::GetFontSize();
                ImGui::SetNextWindowPos(
                    ImVec2{ display.x * 0.5F, display.y * 0.5F }, ImGuiCond_Appearing, ImVec2{ 0.5F, 0.5F });
                ImGui::SetNextWindowSize(ImVec2{ kSizeFonts.x * fontSize, kSizeFonts.y * fontSize }, ImGuiCond_Appearing);
                ImGui::SetNextWindowBgAlpha(kBackgroundAlpha);
                if (focusWindow)
                {
                    ImGui::SetNextWindowFocus();
                    focusWindow = false;
                }
                if (ImGui::Begin("Skyrim Multiplayer Settings###Settings", &a_open, kWindowFlags))
                {
                    // The separator and the button row below the tab content.
                    editor.Draw(draft, ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y);
                    ImGui::Separator();
                    DrawFooter();
                }
                ImGui::End();
            }

            void DrawFooter()
            {
                // Another Apply, or a newer snapshot, may have replaced the live settings.
                live = Config::Get();
                const bool changed = draft != *live;
                const bool invalid = editor.HasInvalidFields();
                if (changed)
                {
                    status = Status::kNone;
                }

                ImGui::BeginDisabled(!changed || invalid);
                if (ImGui::Button("Apply"))
                {
                    status = Config::Apply(draft) ? Status::kSaved : Status::kNotSaved;
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(!changed && !invalid);
                if (ImGui::Button("Revert"))
                {
                    Replace(*live);
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(draft == defaults && !invalid);
                if (ImGui::Button("Defaults"))
                {
                    Replace(defaults);
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Close"))
                {
                    WindowManager::ReleaseFocus(*this);
                }

                ImGui::SameLine();
                if (invalid)
                {
                    ImGui::TextColored(kErrorColor, "Fix the values in red to apply.");
                }
                else if (status == Status::kSaved)
                {
                    ImGui::TextColored(kSuccessColor, "Applied and saved.");
                }
                else if (status == Status::kNotSaved)
                {
                    ImGui::TextColored(kErrorColor, "Applied, but skyrim-mp.ini could not be saved.");
                }
                else if (changed)
                {
                    ImGui::TextColored(kNoteColor, "Unapplied changes.");
                }
            }

            const Config::PluginConfig defaults;
            std::shared_ptr<const Config::PluginConfig> live;
            Config::PluginConfig draft;
            SettingsEditor editor;
            Status status{ Status::kNone };
            bool wasFocused{ false };
            bool focusWindow{ false };
        };
    }

    std::unique_ptr<Window> CreateSettingsWindow()
    {
        return std::make_unique<SettingsWindow>();
    }
}
