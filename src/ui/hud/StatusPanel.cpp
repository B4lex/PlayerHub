#include "ui/hud/StatusPanel.h"
#include "config/PluginConfig.h"
#include "network/ConnectionMonitor.h"
#include "network/ServerClient.h"
#include "network/TrafficMeter.h"
#include "runtime/GameSession.h"
#include "runtime/PluginTime.h"
#include "ui/Layout.h"

#include <imgui.h>

#include <chrono>
#include <optional>

namespace UI
{
    namespace
    {
        constexpr auto kCorner = ScreenCorner::kTopRight;
        constexpr float kMarginPixels = 12.0F;
        constexpr float kBackgroundAlpha = 0.35F;
        constexpr long long kGoodLatencyMilliseconds = 80;
        constexpr long long kFairLatencyMilliseconds = 150;
        constexpr double kBytesPerKilobyte = 1000.0;
        // UTF-8 for the arrows, since the build does not set the source charset.
        constexpr const char* kDownloadArrow = "\xE2\x86\x93";
        constexpr const char* kUploadArrow = "\xE2\x86\x91";

        constexpr ImVec4 kGoodColor{ 0.45F, 0.85F, 0.45F, 1.0F };
        constexpr ImVec4 kFairColor{ 0.95F, 0.80F, 0.30F, 1.0F };
        constexpr ImVec4 kPoorColor{ 0.95F, 0.40F, 0.35F, 1.0F };
        constexpr ImVec4 kMutedColor{ 0.70F, 0.70F, 0.70F, 1.0F };

        constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_AlwaysAutoResize;

        void DrawPing(std::optional<std::chrono::milliseconds> a_latency)
        {
            if (!a_latency)
            {
                ImGui::TextColored(kMutedColor, "Ping -- ms");
                return;
            }

            const auto milliseconds = static_cast<long long>(a_latency->count());
            const auto& color = milliseconds <= kGoodLatencyMilliseconds ? kGoodColor
                : milliseconds <= kFairLatencyMilliseconds               ? kFairColor
                                                                         : kPoorColor;
            ImGui::TextColored(color, "Ping %lld ms", milliseconds);
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
                ImGui::TextColored(kMutedColor, "Plugin -- ms");
                return;
            }

            const std::chrono::duration<double, std::milli> milliseconds = *a_average;
            ImGui::TextColored(kMutedColor, "Plugin %.2f ms", milliseconds.count());
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
        ImGui::SetNextWindowBgAlpha(kBackgroundAlpha);
        if (!ImGui::Begin("##StatusPanel", nullptr, kWindowFlags))
        {
            ImGui::End();
            return;
        }

        if (!Network::IsConnected())
        {
            ImGui::TextColored(kPoorColor, "Disconnected");
        }
        else
        {
            DrawPing(Network::GetLatency());
            DrawTraffic(Network::GetTrafficRates());
        }
        DrawPluginTime(PluginTime::GetAverageFrameTime());

        ImGui::End();
    }

    std::unique_ptr<Window> CreateStatusPanel()
    {
        return std::make_unique<StatusPanel>();
    }
}
