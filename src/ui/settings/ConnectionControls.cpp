#include "ui/settings/ConnectionControls.h"
#include "network/ConnectionMonitor.h"

#include <imgui.h>

#include <algorithm>
#include <optional>
#include <string>

namespace UI::ConnectionControls
{
    namespace
    {
        using Network::ConnectionState;

        constexpr ImVec4 kConnectedColor{ 0.45F, 0.85F, 0.45F, 1.0F };
        constexpr ImVec4 kDisconnectedColor{ 0.95F, 0.40F, 0.35F, 1.0F };
        constexpr ImVec4 kHintColor{ 0.70F, 0.70F, 0.70F, 1.0F };
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

        void DrawIndicator(bool a_connected)
        {
            const auto size = ImGui::GetFrameHeight();
            const auto position = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2{ size, size });
            ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2{ position.x + size * 0.5F, position.y + size * 0.5F },
                size * kIndicatorRadius, ImGui::GetColorU32(a_connected ? kConnectedColor : kDisconnectedColor));
        }

        void DrawStatus(ConnectionState a_state, bool a_refused)
        {
            switch (a_state)
            {
                case ConnectionState::kConnected:
                    if (const auto latency = Network::GetLatency())
                    {
                        ImGui::Text("Connected, ping %lld ms", static_cast<long long>(latency->count()));
                    }
                    else
                    {
                        ImGui::TextUnformatted("Connected");
                    }
                    break;
                case ConnectionState::kConnecting:
                    // A refused connection stays wanted but is not retried.
                    ImGui::TextUnformatted(a_refused ? "Not connected" : "Connecting...");
                    break;
                case ConnectionState::kDisconnecting:
                    ImGui::TextUnformatted("Disconnecting...");
                    break;
                default:
                    ImGui::TextUnformatted("Disconnected");
                    break;
            }
        }

        void DrawMessage(const ImVec4& a_color, const char* a_text)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, a_color);
            ImGui::PushTextWrapPos(0.0F);
            ImGui::TextUnformatted(a_text);
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
    }

    void Draw()
    {
        const auto state = Network::GetConnectionState();
        const bool needsNickname = state == ConnectionState::kDisconnected && !Network::HasNickname();
        const auto refusal
            = state == ConnectionState::kConnecting ? Network::GetConnectionRefusal() : std::nullopt;

        ImGui::BeginDisabled(state == ConnectionState::kDisconnecting || needsNickname);
        if (ImGui::Button(GetButtonLabel(state), ImVec2{ GetButtonWidth(), 0.0F }))
        {
            Network::SetConnectionEnabled(state == ConnectionState::kDisconnected);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        DrawIndicator(state == ConnectionState::kConnected);
        ImGui::SameLine(0.0F, ImGui::GetStyle().ItemInnerSpacing.x);
        DrawStatus(state, refusal.has_value());

        if (refusal)
        {
            DrawMessage(kDisconnectedColor, refusal->message.c_str());
        }
        else if (needsNickname)
        {
            DrawMessage(kHintColor, "Enter a nickname and apply it to connect.");
        }
        else if (state == ConnectionState::kConnected)
        {
            if (const auto rejection = Network::GetNicknameRejection())
            {
                DrawMessage(kDisconnectedColor, rejection->c_str());
            }
        }
    }
}
