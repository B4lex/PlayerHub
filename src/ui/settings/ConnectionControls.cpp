#include "ui/settings/ConnectionControls.h"
#include "config/PluginConfig.h"
#include "network/ConnectionMonitor.h"
#include "ui/Theme.h"
#include "ui/widgets/Controls.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>

namespace UI::ConnectionControls
{
    namespace
    {
        using Network::ConnectionState;

        constexpr float kMinimumButtonWidthFonts = 7.0F;
        constexpr float kIndicatorRadiusFonts = 0.28F;
        constexpr float kIndicatorGapFonts = 0.5F;
        constexpr float kPulsesPerSecond = 0.8F;
        constexpr float kTwoPi = 6.2831853F;
        // The label changes, so the ID after ### keeps the button's identity.
        constexpr const char* kConnectLabel = "Connect###Connection";
        constexpr const char* kCancelLabel = "Cancel###Connection";
        constexpr const char* kDisconnectLabel = "Disconnect###Connection";
        constexpr const char* kDisconnectingLabel = "Disconnecting...###Connection";
        constexpr const char* kInvalidDisplayNameText = "Invalid display name.";
        constexpr const char* kDisconnectToRenameText = "Disconnect to change the display name";
        constexpr const char* kCancelToRenameText = "Cancel the connection to change the display name";
        constexpr const char* kNoEndpointText = "Enter a server URL below, or switch Custom Server off, to connect.";

        struct Status
        {
            ConnectionState state;
            std::optional<Network::Refusal> refusal;
        };

        Status GetStatus()
        {
            const auto state = Network::GetConnectionState();
            return Status{ .state = state,
                .refusal = state == ConnectionState::kDisconnected ? Network::GetConnectionRefusal() : std::nullopt };
        }

        const char* GetStatusText(ConnectionState a_state)
        {
            switch (a_state)
            {
                case ConnectionState::kConnected:
                    return "Connected";
                case ConnectionState::kConnecting:
                    return "Connecting...";
                case ConnectionState::kDisconnecting:
                    return "Disconnecting...";
                default:
                    return "Disconnected";
            }
        }

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

        void DrawIndicator(const Status& a_status)
        {
            auto color = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
            if (a_status.state == ConnectionState::kConnected)
            {
                color = Theme::kPositive;
            }
            else if (a_status.refusal)
            {
                color = Theme::kNegative;
            }
            else if (a_status.state == ConnectionState::kConnecting)
            {
                color = Theme::kWarning;
                color.w = 0.65F + 0.35F * std::sin(static_cast<float>(ImGui::GetTime()) * kPulsesPerSecond * kTwoPi);
            }

            const auto fontSize = ImGui::GetFontSize();
            const auto radius = kIndicatorRadiusFonts * fontSize;
            const auto height = ImGui::GetTextLineHeight();
            const auto position = ImGui::GetCursorScreenPos();
            ImGui::Dummy(ImVec2{ radius * 2.0F, height });
            ImGui::GetWindowDrawList()->AddCircleFilled(
                ImVec2{ position.x + radius, position.y + height * 0.5F }, radius, ImGui::GetColorU32(color));
            ImGui::SameLine(0.0F, std::floor(kIndicatorGapFonts * fontSize));
        }

        bool HasServerEndpoint()
        {
            const auto config = Config::Get();
            return !Config::GetServerEndpoint(config->connection).empty();
        }

        void DrawMessage(std::string_view a_text, const ImVec4& a_color)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, a_color);
            ImGui::PushTextWrapPos(0.0F);
            ImGui::TextUnformatted(a_text.data(), a_text.data() + a_text.size());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
    }

    float GetButtonWidth()
    {
        float textWidth = 0.0F;
        for (const auto* label : { kConnectLabel, kCancelLabel, kDisconnectLabel, kDisconnectingLabel })
        {
            textWidth = (std::max)(textWidth, ImGui::CalcTextSize(label, nullptr, true).x);
        }
        return (std::max)(textWidth + ImGui::GetStyle().FramePadding.x * 2.0F,
            kMinimumButtonWidthFonts * ImGui::GetFontSize());
    }

    void DrawButton(bool a_displayNameEntered)
    {
        const auto state = Network::GetConnectionState();
        const bool canConnect = state == ConnectionState::kDisconnected && a_displayNameEntered && HasServerEndpoint();
        const auto* label = GetButtonLabel(state);
        const ImVec2 size{ GetButtonWidth(), 0.0F };

        ImGui::BeginDisabled(
            state == ConnectionState::kDisconnecting || (state == ConnectionState::kDisconnected && !canConnect));
        bool pressed = false;
        if (state == ConnectionState::kConnected)
        {
            pressed = Controls::DangerButton(label, size);
        }
        else if (canConnect)
        {
            pressed = Controls::PrimaryButton(label, size);
        }
        else
        {
            pressed = ImGui::Button(label, size);
        }
        ImGui::EndDisabled();

        if (pressed)
        {
            Network::SetConnectionEnabled(state == ConnectionState::kDisconnected);
        }
    }

    const char* GetDisplayNameLock()
    {
        switch (Network::GetConnectionState())
        {
            case ConnectionState::kDisconnected:
                return nullptr;
            case ConnectionState::kConnecting:
                return kCancelToRenameText;
            default:
                return kDisconnectToRenameText;
        }
    }

    void DrawNotice(bool a_invalidText, std::string_view a_help)
    {
        if (a_invalidText)
        {
            DrawMessage(kInvalidDisplayNameText, Theme::kNegative);
            return;
        }
        const auto status = GetStatus();
        if (status.refusal)
        {
            DrawMessage(status.refusal->message, Theme::kNegative);
            return;
        }
        if (status.state == ConnectionState::kDisconnected && !HasServerEndpoint())
        {
            DrawMessage(kNoEndpointText, Theme::kWarning);
            return;
        }
        DrawMessage(a_help, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    }

    void DrawSummary(float a_width)
    {
        const auto status = GetStatus();
        const auto left = ImGui::GetCursorPosX();
        DrawIndicator(status);
        ImGui::TextUnformatted(GetStatusText(status.state));

        const auto latency = status.state == ConnectionState::kConnected ? Network::GetLatency() : std::nullopt;
        if (!latency)
        {
            return;
        }
        char ping[24];
        std::snprintf(ping, sizeof(ping), "%lld ms", static_cast<long long>(latency->count()));
        ImGui::SameLine(left + a_width - ImGui::CalcTextSize(ping).x);
        ImGui::TextDisabled("%s", ping);
    }
}
