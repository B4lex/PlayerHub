#include "ui/hud/VoiceIndicator.h"
#include "config/PluginConfig.h"
#include "runtime/GameSession.h"
#include "sync/voicechat/VoiceChatSync.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"
#include "ui/hud/GameView.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace UI
{
    namespace
    {
        constexpr float kSizeFonts = 2.0F;
        constexpr float kOutlineAlpha = 0.8F;
        // U+E008 of the logo font.
        constexpr const char* kMicrophoneGlyph = "\xEE\x80\x88";

        struct Place
        {
            float horizontal{ 0.0F };
            float vertical{ 0.0F };
        };

        // The settings window draws after the indicator, so a preview shows a frame later.
        std::optional<Place> preview;

        std::optional<Place> GetHeardPlace()
        {
            if (!GameSession::IsInGame() || !IsGameViewShown())
            {
                return std::nullopt;
            }
            const auto status = VoiceChatSync::GetLocalStatus();
            if (!status.active || !status.transmitting)
            {
                return std::nullopt;
            }
            const auto config = Config::Get();
            const auto& voice = config->voice;
            if (!voice.indicator)
            {
                return std::nullopt;
            }
            return Place{ voice.indicatorPositionHorizontal, voice.indicatorPositionVertical };
        }

        class VoiceIndicator final : public Window
        {
        public:
            void Draw(const WindowInput&) override
            {
                auto place = std::exchange(preview, std::nullopt);
                if (!place)
                {
                    place = GetHeardPlace();
                }
                auto* const font = Fonts::GetLogo();
                if (!place || !font)
                {
                    return;
                }
                const auto& display = ImGui::GetIO().DisplaySize;
                const auto fontSize = ImGui::GetFontSize();
                const auto size = std::round(kSizeFonts * fontSize);
                const ImVec2 min{ std::clamp(std::round(place->horizontal * 0.01F * display.x), 0.0F,
                                      (std::max)(display.x - size, 0.0F)),
                    std::clamp(std::round(display.y - place->vertical * 0.01F * display.y) - size, 0.0F,
                        (std::max)(display.y - size, 0.0F)) };
                auto* drawList = ImGui::GetBackgroundDrawList();
                const auto outline = (std::max)(1.0F, std::floor(fontSize / 16.0F));
                const auto shadow = ImGui::GetColorU32(ImVec4{ 0.0F, 0.0F, 0.0F, kOutlineAlpha });
                drawList->AddText(font, size, ImVec2{ min.x + outline, min.y + outline }, shadow, kMicrophoneGlyph);
                drawList->AddText(font, size, min, ImGui::GetColorU32(Theme::GetAccent()), kMicrophoneGlyph);
            }
        };
    }

    std::unique_ptr<Window> CreateVoiceIndicator()
    {
        return std::make_unique<VoiceIndicator>();
    }

    void PreviewVoiceIndicator(float a_horizontal, float a_vertical)
    {
        preview = Place{ a_horizontal, a_vertical };
    }
}
