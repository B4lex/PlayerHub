#include "ui/hud/VoiceIndicator.h"
#include "runtime/GameSession.h"
#include "sync/voice/VoiceSync.h"
#include "ui/Theme.h"
#include "ui/hud/GameView.h"
#include "ui/widgets/VoiceGlyphs.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace UI
{
    namespace
    {
        // Above the vanilla compass's left end, out of the way of the chat and the notices.
        constexpr float kLeftFraction = 0.32F;
        constexpr float kTopFonts = 1.0F;
        constexpr float kSizeFonts = 1.25F;
        constexpr float kOutlineAlpha = 0.8F;

        class VoiceIndicator final : public Window
        {
        public:
            void Draw(const WindowInput&) override
            {
                if (!GameSession::IsInGame() || !IsGameViewShown())
                {
                    return;
                }
                const auto status = VoiceSync::GetLocalStatus();
                if (!status.active || (!status.transmitting && !status.microphoneOff))
                {
                    return;
                }
                const auto& display = ImGui::GetIO().DisplaySize;
                const auto fontSize = ImGui::GetFontSize();
                const auto size = std::round(kSizeFonts * fontSize);
                const ImVec2 min{ std::round(display.x * kLeftFraction), std::round(kTopFonts * fontSize) };
                auto* drawList = ImGui::GetBackgroundDrawList();
                const auto outline = (std::max)(1.0F, std::floor(fontSize / 16.0F));
                const auto shadow = ImGui::GetColorU32(ImVec4{ 0.0F, 0.0F, 0.0F, kOutlineAlpha });
                const auto color = status.microphoneOff ? ImGui::GetColorU32(Theme::kNegative)
                                                        : ImGui::GetColorU32(Theme::GetAccent());
                VoiceGlyphs::DrawMicrophone(*drawList, ImVec2{ min.x + outline, min.y + outline }, size, shadow,
                    status.microphoneOff);
                VoiceGlyphs::DrawMicrophone(*drawList, min, size, color, status.microphoneOff);
            }
        };
    }

    std::unique_ptr<Window> CreateVoiceIndicator()
    {
        return std::make_unique<VoiceIndicator>();
    }
}
