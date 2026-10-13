#include "ui/hud/DisplayNameHint.h"
#include "config/PluginConfig.h"
#include "config/SettingsSchema.h"
#include "runtime/GameSession.h"
#include "ui/MenuFrame.h"
#include "ui/Theme.h"
#include "ui/WindowManager.h"
#include "ui/hud/GameView.h"

#include <imgui.h>

#include <string_view>

namespace UI
{
    namespace
    {
        void DrawWord(std::string_view a_text, bool a_accent)
        {
            if (a_accent)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::GetAccent());
            }
            ImGui::TextUnformatted(a_text.data(), a_text.data() + a_text.size());
            if (a_accent)
            {
                ImGui::PopStyleColor();
            }
        }

        class DisplayNameHint final : public Window
        {
        public:
            void Draw(const WindowInput&) override
            {
                if (!GameSession::IsInGame() || WindowManager::HasFocus())
                {
                    return;
                }
                const auto config = Config::Get();
                if (!config->general.displayName.empty() || !IsGameViewShown())
                {
                    return;
                }
                const Theme::Scope theme;
                if (MenuFrame::BeginPrompt("##DisplayNameHint"))
                {
                    const auto space = ImGui::CalcTextSize(" ").x;
                    DrawWord("Press", false);
                    ImGui::SameLine(0.0F, space);
                    DrawWord(Config::FormatKeyLabel(config->controls.settingsKey), true);
                    ImGui::SameLine(0.0F, space);
                    DrawWord("and enter a display name on the", false);
                    ImGui::SameLine(0.0F, space);
                    DrawWord("General", true);
                    ImGui::SameLine(0.0F, space);
                    DrawWord("tab to connect", false);
                }
                ImGui::End();
            }
        };
    }

    std::unique_ptr<Window> CreateDisplayNameHint()
    {
        return std::make_unique<DisplayNameHint>();
    }
}
