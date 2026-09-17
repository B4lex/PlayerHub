#include "ui/hud/PingIndicator.h"
#include "network/LatencyProbe.h"
#include "network/PacketDelay.h"
#include "network/SocketIOClient.h"
#include "ui/Layout.h"

#include <imgui.h>

#include <chrono>
#include <optional>

namespace UI::PingIndicator
{
    namespace
    {
        constexpr auto kCorner = ScreenCorner::kTopRight;
        constexpr float kMarginPixels = 12.0F;
        constexpr float kBackgroundAlpha = 0.35F;
        constexpr long long kGoodLatencyMilliseconds = 80;
        constexpr long long kFairLatencyMilliseconds = 150;

        constexpr ImVec4 kGoodColor{ 0.45F, 0.85F, 0.45F, 1.0F };
        constexpr ImVec4 kFairColor{ 0.95F, 0.80F, 0.30F, 1.0F };
        constexpr ImVec4 kPoorColor{ 0.95F, 0.40F, 0.35F, 1.0F };
        constexpr ImVec4 kMutedColor{ 0.70F, 0.70F, 0.70F, 1.0F };

        constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs
            | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing
            | ImGuiWindowFlags_AlwaysAutoResize;

        void DrawMilliseconds(const char* a_label, std::optional<std::chrono::milliseconds> a_value)
        {
            if (!a_value)
            {
                ImGui::TextColored(kMutedColor, "%s -- ms", a_label);
                return;
            }

            const auto milliseconds = static_cast<long long>(a_value->count());
            const auto& color = milliseconds <= kGoodLatencyMilliseconds ? kGoodColor
                : milliseconds <= kFairLatencyMilliseconds               ? kFairColor
                                                                         : kPoorColor;
            ImGui::TextColored(color, "%s %lld ms", a_label, milliseconds);
        }
    }

    void Draw()
    {
        SetNextWindowCorner(kCorner, kMarginPixels);
        ImGui::SetNextWindowBgAlpha(kBackgroundAlpha);
        if (!ImGui::Begin("##PingIndicator", nullptr, kWindowFlags))
        {
            ImGui::End();
            return;
        }

        if (!Network::IsConnected())
        {
            ImGui::TextColored(kPoorColor, "Offline");
        }
        else
        {
            DrawMilliseconds("Ping", Network::GetLatency());
            DrawMilliseconds("Delay", Network::GetRemotePacketDelay());
        }

        ImGui::End();
    }
}
