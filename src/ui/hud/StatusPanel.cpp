#include "ui/hud/StatusPanel.h"
#include "config/PluginConfig.h"
#include "network/ConnectionMonitor.h"
#include "network/ServerClient.h"
#include "network/TrafficMeter.h"
#include "runtime/GameSession.h"
#include "runtime/PluginTime.h"
#include "text/Text.h"
#include "ui/Fonts.h"
#include "ui/Layout.h"
#include "ui/widgets/TextShadow.h"

#include <imgui.h>

#include <chrono>
#include <optional>

namespace UI
{
    namespace
    {
        constexpr auto kCorner = ScreenCorner::kTopRight;
        constexpr float kMarginPixels = 12.0F;
        constexpr long long kGoodLatencyMilliseconds = 80;
        constexpr long long kFairLatencyMilliseconds = 150;
        constexpr double kBytesPerKilobyte = 1000.0;
        constexpr const char* kDownloadArrow = "\xE2\x86\x93";
        constexpr const char* kUploadArrow = "\xE2\x86\x91";

        constexpr ImVec4 kGoodColor{ 0.45F, 0.85F, 0.45F, 1.0F };
        constexpr ImVec4 kFairColor{ 0.95F, 0.80F, 0.30F, 1.0F };
        constexpr ImVec4 kPoorColor{ 0.95F, 0.40F, 0.35F, 1.0F };
        constexpr ImVec4 kMutedColor{ 0.70F, 0.70F, 0.70F, 1.0F };

        constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground;

        void DrawPing(std::optional<std::chrono::milliseconds> a_latency)
        {
            if (!a_latency)
            {
                ImGui::TextColored(kMutedColor, "%s -- ms", Text::GetCString(Text::ID::kStatusPing));
                return;
            }

            const auto milliseconds = static_cast<long long>(a_latency->count());
            const auto& color = milliseconds <= kGoodLatencyMilliseconds ? kGoodColor
                : milliseconds <= kFairLatencyMilliseconds               ? kFairColor
                                                                         : kPoorColor;
            ImGui::TextColored(color, "%s %lld ms", Text::GetCString(Text::ID::kStatusPing), milliseconds);
        }

        void DrawTraffic(std::optional<Network::TrafficRates> a_rates)
        {
            if (!a_rates)
            {
                ImGui::TextColored(kMutedColor, "%s -- KB/s", kDownloadArrow);
                ImGui::TextColored(kMutedColor, "%s -- KB/s", kUploadArrow);
                return;
            }

            ImGui::TextColored(
                kMutedColor, "%s %.1f KB/s", kDownloadArrow, a_rates->receivedBytesPerSecond / kBytesPerKilobyte);
            ImGui::TextColored(
                kMutedColor, "%s %.1f KB/s", kUploadArrow, a_rates->sentBytesPerSecond / kBytesPerKilobyte);
        }

        void DrawPluginTime(std::optional<std::chrono::nanoseconds> a_average)
        {
            if (!a_average)
            {
                ImGui::TextColored(kMutedColor, "%s -- ms", Text::GetCString(Text::ID::kStatusPlugin));
                return;
            }

            const std::chrono::duration<double, std::milli> milliseconds = *a_average;
            ImGui::TextColored(
                kMutedColor, "%s %.2f ms", Text::GetCString(Text::ID::kStatusPlugin), milliseconds.count());
        }

        class StatusPanel final : public Window
        {
        public:
            void Draw(const WindowInput&) override;
        };
    }

    void StatusPanel::Draw(const WindowInput&)
    {
        if (!GameSession::IsInGame() || !Config::Get()->advanced.statusPanel)
        {
            return;
        }

        SetNextWindowCorner(kCorner, kMarginPixels);
        ImGui::PushFont(Fonts::GetSystemBold(), 0.0F);
        if (ImGui::Begin("##StatusPanel", nullptr, kWindowFlags))
        {
            const bool connected = Network::IsConnected();
            const auto latency = Network::GetLatency();
            const auto rates = Network::GetTrafficRates();
            const auto pluginTime = PluginTime::GetAverageFrameTime();
            TextShadow::Draw([&] {
                if (!connected)
                {
                    ImGui::TextColored(kPoorColor, "%s", Text::GetCString(Text::ID::kConnectionDisconnected));
                }
                else
                {
                    DrawPing(latency);
                    DrawTraffic(rates);
                }
                DrawPluginTime(pluginTime);
            });
        }
        ImGui::End();
        ImGui::PopFont();
    }

    std::unique_ptr<Window> CreateStatusPanel()
    {
        return std::make_unique<StatusPanel>();
    }
}
