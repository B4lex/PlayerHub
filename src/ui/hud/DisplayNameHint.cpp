#include "ui/hud/DisplayNameHint.h"
#include "config/PluginConfig.h"
#include "config/SettingsSchema.h"
#include "runtime/GameSession.h"
#include "runtime/VanillaIntro.h"
#include "text/Text.h"
#include "ui/MenuFrame.h"
#include "ui/Theme.h"
#include "ui/WindowManager.h"
#include "ui/hud/GameView.h"
#include "ui/widgets/Controls.h"

#include <imgui.h>

#include <array>
#include <string_view>

namespace UI
{
    namespace
    {
        class DisplayNameHint final : public Window
        {
        public:
            void Draw(const WindowInput&) override
            {
                if (!GameSession::IsInGame() || VanillaIntro::IsPlaying() || WindowManager::HasFocus())
                {
                    return;
                }
                const auto config = Config::Get();
                if (!config->connection.displayName.empty() || !IsGameViewShown())
                {
                    return;
                }
                const Theme::Scope theme;
                if (MenuFrame::BeginPrompt("##DisplayNameHint"))
                {
                    const std::array<std::string_view, 2> arguments{
                        Config::FormatKeyLabel(config->controls.settingsKey), Text::Get(Text::Id::kSectionConnection)
                    };
                    Controls::AccentedText(Text::Get(Text::Id::kDisplayNameHint), arguments);
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
