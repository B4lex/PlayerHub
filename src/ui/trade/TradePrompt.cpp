#include "ui/trade/TradePrompt.h"
#include "config/PluginConfig.h"
#include "config/SettingsSchema.h"
#include "sync/trade/TradeSync.h"
#include "ui/MenuFrame.h"
#include "ui/Theme.h"
#include "ui/WindowManager.h"

#include <imgui.h>

#include <atomic>
#include <cmath>
#include <string>

namespace UI
{
    namespace
    {
        using Phase = TradeSession::Phase;

        // Below the compass.
        constexpr float kTopFonts = 4.5F;
        constexpr float kBackgroundAlpha = 0.85F;
        constexpr ImGuiWindowFlags kWindowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize
            | ImGuiWindowFlags_NoNav;

        class TradePrompt final : public Window
        {
        public:
            [[nodiscard]] bool IsFocusKey(std::uint32_t a_scanCode) const override
            {
                const auto phase = TradeSync::GetPhase();
                const auto& controls = Config::Get()->controls;
                return (phase == Phase::kInvited && a_scanCode == controls.tradeAcceptKey)
                    || ((phase == Phase::kInvited || phase == Phase::kInviting)
                        && a_scanCode == controls.tradeDeclineKey);
            }

            void OnFocusKey(std::uint32_t a_scanCode) override { pressedKey.store(a_scanCode, std::memory_order_release); }

            void Draw(const WindowInput& a_input) override
            {
                static_cast<void>(TradeSync::CopyView(revision, view));
                if (a_input.focused)
                {
                    Answer();
                    WindowManager::ReleaseFocus(*this);
                }
                if (view.phase != Phase::kRequesting && view.phase != Phase::kInviting && view.phase != Phase::kInvited)
                {
                    return;
                }
                DrawPrompt();
            }

        private:
            void Answer()
            {
                const auto key = pressedKey.exchange(0, std::memory_order_acq_rel);
                // 0 also matches an unbound action.
                if (key == 0)
                {
                    return;
                }
                const auto& controls = Config::Get()->controls;
                if (view.phase == Phase::kInvited && key == controls.tradeAcceptKey)
                {
                    TradeSync::AnswerInvitation(true);
                }
                else if (view.phase == Phase::kInvited && key == controls.tradeDeclineKey)
                {
                    TradeSync::AnswerInvitation(false);
                }
                else if (view.phase == Phase::kInviting && key == controls.tradeDeclineKey)
                {
                    TradeSync::Cancel(Multiplayer::TradeCloseReason::kCancelled);
                }
            }

            void DrawPrompt()
            {
                const Theme::Scope theme;
                const auto& display = ImGui::GetIO().DisplaySize;
                const auto fontSize = ImGui::GetFontSize();
                ImGui::SetNextWindowPos(
                    ImVec2{ display.x * 0.5F, std::floor(kTopFonts * fontSize) }, ImGuiCond_Always, ImVec2{ 0.5F, 0.0F });
                const auto frame = MenuFrame::GetMetrics();
                const auto& padding = ImGui::GetStyle().WindowPadding;
                const auto inset = std::floor((frame.inset + frame.corner) * 0.5F);
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{ padding.x + inset, padding.y + inset });
                const bool visible = ImGui::Begin("##TradePrompt", nullptr, kWindowFlags | ImGuiWindowFlags_NoBackground);
                ImGui::PopStyleVar();
                if (visible)
                {
                    auto background = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
                    background.w = kBackgroundAlpha;
                    MenuFrame::Draw(ImGui::GetColorU32(Theme::GetFrameColor()), ImGui::GetColorU32(background));
                    DrawContent();
                }
                ImGui::End();
            }

            void DrawContent() const
            {
                const auto& controls = Config::Get()->controls;
                const auto& name = view.peer.name;
                switch (view.phase)
                {
                    case Phase::kRequesting:
                        ImGui::Text("Offering a trade to %s...", name.c_str());
                        break;
                    case Phase::kInviting:
                        ImGui::Text("Waiting for %s to answer your trade offer", name.c_str());
                        DrawKey(controls.tradeDeclineKey, "Withdraw", false);
                        DrawCountdown();
                        break;
                    case Phase::kInvited:
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, Theme::GetAccent());
                        ImGui::TextUnformatted(name.c_str());
                        ImGui::PopStyleColor();
                        ImGui::SameLine(0.0F, ImGui::CalcTextSize(" ").x);
                        ImGui::TextUnformatted("wants to trade");
                        const bool accept = DrawKey(controls.tradeAcceptKey, "Accept", false);
                        DrawKey(controls.tradeDeclineKey, "Decline", accept);
                        DrawCountdown();
                        break;
                    }
                    default:
                        break;
                }
            }

            static bool DrawKey(std::uint32_t a_scanCode, const char* a_action, bool a_sameLine)
            {
                const auto key = Config::FormatKeyLabel(a_scanCode);
                if (key.empty())
                {
                    return false;
                }
                if (a_sameLine)
                {
                    ImGui::SameLine();
                }
                ImGui::PushStyleColor(ImGuiCol_Text, Theme::GetAccent());
                ImGui::Text("[%.*s]", static_cast<int>(key.size()), key.data());
                ImGui::PopStyleColor();
                ImGui::SameLine(0.0F, ImGui::CalcTextSize(" ").x);
                ImGui::TextUnformatted(a_action);
                return true;
            }

            void DrawCountdown() const
            {
                ImGui::SameLine();
                ImGui::TextDisabled("%u s", view.secondsLeft);
            }

            TradeSync::View view;
            std::uint64_t revision{ 0 };
            std::atomic<std::uint32_t> pressedKey{ 0 };
        };
    }

    std::unique_ptr<Window> CreateTradePrompt()
    {
        return std::make_unique<TradePrompt>();
    }
}
