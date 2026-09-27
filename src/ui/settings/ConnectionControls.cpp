#include "ui/settings/ConnectionControls.h"
#include "network/ConnectionMonitor.h"

#include <imgui.h>

#include <algorithm>

namespace UI::ConnectionControls
{
    namespace
    {
        using Network::ConnectionState;

        constexpr ImVec4 kConnectedColor{ 0.45F, 0.85F, 0.45F, 1.0F };
        constexpr ImVec4 kDisconnectedColor{ 0.95F, 0.40F, 0.35F, 1.0F };
        // Of the frame height, which is also the indicator's width.
        constexpr float kIndicatorRadius = 0.3F;
        // The label changes, so the ID after ### keeps the button's identity.
        constexpr const char* kConnectLabel = "Connect###Connection";
        constexpr const char* kCancelLabel = "Cancel###Connection";
        constexpr const char* kDisconnectLabel = "Disconnect###Connection";
        constexpr const char* kDisconnectingLabel = "Disconnecting...###Connection";

        const char* GetButtonLabel(ConnectionState a_state)
        {
            switch (a_state)
            {
                case ConnectionState::kConnected:
                    return kDisconnectLabel;
                case ConnectionState::kConnecting:
                    return kCancelLabel;
                case ConnectionState::kDisconnecting:
                    return kDisconnectingLabel;
                default:
                    return kConnectLabel;
            }
        }

        float GetButtonWidth()
        {
            float textWidth = 0.0F;
            for (const auto* label : { kConnectLabel, kCancelLabel, kDisconnectLabel, kDisconnectingLabel })
            {
                textWidth = (std::max)(textWidth, ImGui::CalcTextSize(label, nullptr, true).x);
            }
            return textWidth + ImGui::GetStyle().FramePadding.x * 2.0F;
        }

        void DrawIndicator(ConnectionState a_state)
        {
            const bool connected = a_state == ConnectionState::kConnected;
            const auto size = ImGui::GetFrameHeight();
            const auto position = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2{ size, size });
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2{ position.x + size * 0.5F, position.y + size * 0.5F },
                size * kIndicatorRadius, ImGui::GetColorU32(connected ? kConnectedColor : kDisconnectedColor));

            if (!ImGui::IsItemHovered())
            {
                return;
            }
            switch (a_state)
            {
                case ConnectionState::kConnected:
                    if (const auto latency = Network::GetLatency())
                    {
                        ImGui::SetTooltip("Connected, ping %lld ms", static_cast<long long>(latency->count()));
                    }
                    else
                    {
                        ImGui::SetTooltip("Connected");
                    }
                    break;
                case ConnectionState::kConnecting:
                    ImGui::SetTooltip("Connecting...");
                    break;
                case ConnectionState::kDisconnecting:
                    ImGui::SetTooltip("Disconnecting...");
                    break;
                default:
                    ImGui::SetTooltip("Disconnected");
                    break;
            }
        }
    }

    float GetWidth()
    {
        return ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x + GetButtonWidth();
    }

    void Draw()
    {
        const auto state = Network::GetConnectionState();
        DrawIndicator(state);
        ImGui::SameLine();
        ImGui::BeginDisabled(state == ConnectionState::kDisconnecting);
        if (ImGui::Button(GetButtonLabel(state), ImVec2{ GetButtonWidth(), 0.0F }))
        {
            Network::SetConnectionEnabled(state == ConnectionState::kDisconnected);
        }
        ImGui::EndDisabled();
    }
}
