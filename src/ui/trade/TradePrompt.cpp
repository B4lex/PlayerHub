#include "ui/trade/TradePrompt.h"
#include "config/PluginConfig.h"
#include "config/SettingsSchema.h"
#include "sync/trade/TradeSync.h"
#include "ui/MenuFrame.h"
#include "ui/Theme.h"
#include "ui/WindowManager.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <format>
#include <string_view>

namespace UI
{
    namespace
    {
        using Phase = TradeSession::Phase;

        constexpr float kHintGapFonts = 0.5F;

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
                if (MenuFrame::BeginPrompt("##TradePrompt"))
                {
                    DrawContent();
                }
                ImGui::End();
            }

            void DrawContent() const
            {
                const auto& controls = Config::Get()->controls;
                const std::string_view name = view.peer.name;
                const auto space = ImGui::CalcTextSize(" ").x;
                Line message;
                Line hints;
                switch (view.phase)
                {
                    // Requesting looks like inviting so the prompt keeps its size when the server's
                    // answer arrives.
                    case Phase::kRequesting:
                    case Phase::kInviting:
                        message.Add("Waiting for", Tone::kNormal, 0.0F);
                        message.Add(name, Tone::kAccent, space);
                        message.Add("to answer your trade offer", Tone::kNormal, space);
                        AddKey(hints, controls.tradeDeclineKey, "Withdraw");
                        break;
                    case Phase::kInvited:
                        message.Add(name, Tone::kAccent, 0.0F);
                        message.Add("wants to trade with you", Tone::kNormal, space);
                        AddKey(hints, controls.tradeAcceptKey, "Accept");
                        AddKey(hints, controls.tradeDeclineKey, "Decline");
                        break;
                    default:
                        return;
                }

                std::array<char, 16> seconds{};
                const auto secondsEnd = std::format_to_n(seconds.data(), seconds.size(), "{} s", view.secondsLeft).out;
                std::array<char, 16> longest{};
                const auto longestEnd = std::format_to_n(longest.data(), longest.size(), "{} s",
                    static_cast<std::uint32_t>(std::ceil(TradeSession::kInviteSeconds))).out;
                // Laid out at the width of the longest count, so the centered line does not shift
                // as the count loses a digit.
                hints.Add(std::string_view{ seconds.data(), secondsEnd }, Tone::kDisabled,
                    hints.count == 0 ? 0.0F : ImGui::GetStyle().ItemSpacing.x,
                    ImGui::CalcTextSize(longest.data(), longestEnd).x);

                const auto width = (std::max)(message.GetWidth(), hints.GetWidth());
                message.Draw(width);
                ImGui::Dummy(ImVec2{ 0.0F, std::floor(kHintGapFonts * ImGui::GetFontSize()) });
                hints.Draw(width);
            }

            enum class Tone
            {
                kNormal,
                kAccent,
                kDisabled
            };

            // A line of text pieces measured before it is drawn, so it can be centered.
            struct Line
            {
                struct Piece
                {
                    std::string_view text;
                    Tone tone{ Tone::kNormal };
                    // Space before the piece.
                    float gap{ 0.0F };
                    float width{ 0.0F };
                };

                void Add(std::string_view a_text, Tone a_tone, float a_gap, float a_minimumWidth = 0.0F)
                {
                    if (count == pieces.size())
                    {
                        return;
                    }
                    const auto textWidth = ImGui::CalcTextSize(a_text.data(), a_text.data() + a_text.size()).x;
                    pieces[count++] = Piece{
                        .text = a_text, .tone = a_tone, .gap = a_gap, .width = (std::max)(textWidth, a_minimumWidth)
                    };
                }

                [[nodiscard]] float GetWidth() const
                {
                    float width = 0.0F;
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        width += pieces[i].gap + pieces[i].width;
                    }
                    return width;
                }

                void Draw(float a_width) const
                {
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::floor((a_width - GetWidth()) * 0.5F));
                    for (std::size_t i = 0; i < count; ++i)
                    {
                        const auto& piece = pieces[i];
                        if (i > 0)
                        {
                            ImGui::SameLine(0.0F, piece.gap);
                        }
                        if (piece.tone != Tone::kNormal)
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text,
                                piece.tone == Tone::kAccent ? Theme::GetAccent()
                                                            : ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                        }
                        ImGui::TextUnformatted(piece.text.data(), piece.text.data() + piece.text.size());
                        if (piece.tone != Tone::kNormal)
                        {
                            ImGui::PopStyleColor();
                        }
                        const auto drawn = ImGui::GetItemRectSize().x;
                        if (piece.width > drawn)
                        {
                            ImGui::SameLine(0.0F, 0.0F);
                            ImGui::Dummy(ImVec2{ piece.width - drawn, 0.0F });
                        }
                    }
                }

                std::array<Piece, 8> pieces{};
                std::size_t count{ 0 };
            };

            static void AddKey(Line& a_line, std::uint32_t a_scanCode, std::string_view a_action)
            {
                const auto key = Config::FormatKeyLabel(a_scanCode);
                if (key.empty())
                {
                    return;
                }
                a_line.Add(key, Tone::kAccent, a_line.count == 0 ? 0.0F : ImGui::GetStyle().ItemSpacing.x);
                a_line.Add(a_action, Tone::kNormal, ImGui::CalcTextSize(" ").x);
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
