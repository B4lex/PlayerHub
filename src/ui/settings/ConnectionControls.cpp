#include "ui/settings/ConnectionControls.h"
#include "config/PluginConfig.h"
#include "network/ConnectionMonitor.h"
#include "protocol/Session.h"
#include "runtime/VanillaIntro.h"
#include "text/Text.h"
#include "ui/Theme.h"
#include "ui/widgets/Controls.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <format>
#include <optional>
#include <string>
#include <string_view>

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
        using Text::Id;

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

        Id GetStatusText(ConnectionState a_state)
        {
            switch (a_state)
            {
                case ConnectionState::kConnected:
                    return Id::kConnectionConnected;
                case ConnectionState::kConnecting:
                    return Id::kConnectionConnecting;
                case ConnectionState::kDisconnecting:
                    return Id::kConnectionDisconnecting;
                default:
                    return Id::kConnectionDisconnected;
            }
        }

        // The label changes, so the ID after ### keeps the button's identity.
        const std::array<std::string, 4>& GetButtonLabels()
        {
            static std::array<std::string, 4> labels;
            static std::optional<Text::Language> language;
            if (language != Text::GetLanguage())
            {
                language = Text::GetLanguage();
                labels = {
                    std::format("{}###Connection", Text::Get(Id::kConnectionConnect)),
                    std::format("{}###Connection", Text::Get(Id::kConnectionCancel)),
                    std::format("{}###Connection", Text::Get(Id::kConnectionDisconnect)),
                    std::format("{}###Connection", Text::Get(Id::kConnectionDisconnecting)),
                };
            }
            return labels;
        }

        const char* GetButtonLabel(ConnectionState a_state)
        {
            std::size_t index = 0;
            switch (a_state)
            {
                case ConnectionState::kConnecting:
                    index = 1;
                    break;
                case ConnectionState::kConnected:
                    index = 2;
                    break;
                case ConnectionState::kDisconnecting:
                    index = 3;
                    break;
                default:
                    break;
            }
            return GetButtonLabels()[index].c_str();
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

        // The field takes no other characters and no more than the maximum, so a short name is the only invalid one.
        const std::string& GetShortDisplayNameText()
        {
            static std::string text;
            static std::optional<Text::Language> language;
            if (language != Text::GetLanguage())
            {
                language = Text::GetLanguage();
                text = Text::FormatPlural(Id::kConnectionShortName, Multiplayer::kMinimumDisplayNameLength);
            }
            return text;
        }

        std::string FormatRefusal(const Network::Refusal& a_refusal)
        {
            switch (a_refusal.reason)
            {
                case Network::RefusalReason::kClientOutdated:
                    return std::string{ Text::Get(Id::kRefusalClientOutdated) };
                case Network::RefusalReason::kServerOutdated:
                    return std::string{ Text::Get(Id::kRefusalServerOutdated) };
                case Network::RefusalReason::kDisplayNameTaken:
                    return std::string{ Text::Get(Id::kRefusalDisplayNameTaken) };
                case Network::RefusalReason::kDisplayNameInvalid:
                    return std::string{ Text::Get(Id::kRefusalDisplayNameInvalid) };
                case Network::RefusalReason::kSessionReplaced:
                    return std::string{ Text::Get(Id::kRefusalSessionReplaced) };
                case Network::RefusalReason::kServerFull:
                    return std::string{ Text::Get(Id::kRefusalServerFull) };
                case Network::RefusalReason::kTooManyNewSessions:
                    return std::string{ Text::Get(Id::kRefusalTooManyNewSessions) };
                case Network::RefusalReason::kAddressFull:
                    return std::string{ Text::Get(Id::kRefusalAddressFull) };
                case Network::RefusalReason::kBanned:
                    return std::string{ Text::Get(Id::kRefusalBanned) };
                case Network::RefusalReason::kKicked:
                    return {};
            }
            return {};
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
        for (const auto& label : GetButtonLabels())
        {
            textWidth = (std::max)(textWidth, ImGui::CalcTextSize(label.c_str(), nullptr, true).x);
        }
        return (std::max)(textWidth + ImGui::GetStyle().FramePadding.x * 2.0F,
            kMinimumButtonWidthFonts * ImGui::GetFontSize());
    }

    bool CanConnect(bool a_displayNameEntered)
    {
        return Network::GetConnectionState() == ConnectionState::kDisconnected && a_displayNameEntered
            && HasServerEndpoint() && !VanillaIntro::IsPlaying();
    }

    void Connect()
    {
        Network::SetConnectionEnabled(true);
    }

    void DrawButton(bool a_displayNameEntered)
    {
        const auto state = Network::GetConnectionState();
        const bool canConnect = CanConnect(a_displayNameEntered);
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
                return Text::GetCString(Id::kConnectionCancelToRename);
            default:
                return Text::GetCString(Id::kConnectionDisconnectToRename);
        }
    }

    const char* GetButtonLock()
    {
        if (Network::GetConnectionState() != ConnectionState::kDisconnected)
        {
            return nullptr;
        }
        if (VanillaIntro::IsPlaying())
        {
            return Text::GetCString(Id::kConnectionGameStage);
        }
        if (!HasServerEndpoint())
        {
            return Text::GetCString(Id::kConnectionNoEndpoint);
        }
        return nullptr;
    }

    void DrawNotice(bool a_shortText)
    {
        if (a_shortText)
        {
            DrawMessage(GetShortDisplayNameText(), Theme::kNegative);
            return;
        }
        const auto status = GetStatus();
        if (const auto text = status.refusal ? FormatRefusal(*status.refusal) : std::string{}; !text.empty())
        {
            DrawMessage(text, Theme::kNegative);
            return;
        }
        // Keeps the line's height, so the rows below do not move when a message appears.
        ImGui::Dummy(ImVec2{ 0.0F, ImGui::GetTextLineHeight() });
    }

    void DrawSummary(float a_width)
    {
        const auto status = GetStatus();
        const auto left = ImGui::GetCursorPosX();
        DrawIndicator(status);
        ImGui::TextUnformatted(Text::GetCString(GetStatusText(status.state)));

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
