#include "ui/trade/TradePrompt.h"
#include "config/PluginConfig.h"
#include "config/SettingsSchema.h"
#include "sync/trade/TradeSync.h"
#include "text/Text.h"
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
                using Text::Id;
                const auto& controls = Config::Get()->controls;
                Line message;
                Line hints;
                switch (view.phase)
                {
                    // Requesting looks like inviting so the prompt keeps its size when the server's
                    // answer arrives.
                    case Phase::kRequesting:
                    case Phase::kInviting:
                        AddMessage(message, Text::Get(Id::kTradePromptWaiting), view.peer.name);
                        AddKey(hints, controls.tradeDeclineKey, Text::Get(Id::kTradePromptWithdraw));
                        break;
                    case Phase::kInvited:
                        AddMessage(message, Text::Get(Id::kTradePromptInvited), view.peer.name);
                        AddKey(hints, controls.tradeAcceptKey, Text::Get(Id::kTradePromptAccept));
                        AddKey(hints, controls.tradeDeclineKey, Text::Get(Id::kTradePromptDecline));
                        break;
                    default:
                        return;
                }

                // Short enough for the small-string buffer, so no allocation per frame.
                const auto seconds = Text::Format(Id::kTradePromptSeconds, view.secondsLeft);
                const auto longest = Text::Format(
                    Id::kTradePromptSeconds, static_cast<std::uint32_t>(std::ceil(TradeSession::kInviteSeconds)));
                // Laid out at the width of the longest count, so the centered line does not shift
                // as the count loses a digit.
                hints.Add(seconds, Tone::kDisabled, hints.count == 0 ? 0.0F : ImGui::GetStyle().ItemSpacing.x,
                    ImGui::CalcTextSize(longest.c_str()).x);

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

            // The name in the accent color where the format has its field.
            static void AddMessage(Line& a_line, std::string_view a_format, std::string_view a_name)
            {
                std::array<Text::Piece, 4> pieces;
                const auto count = Text::Split(a_format, pieces);
                for (std::size_t i = 0; i < count; ++i)
                {
                    const bool name = pieces[i].argument == 0;
                    a_line.Add(name ? a_name : pieces[i].text, name ? Tone::kAccent : Tone::kNormal, 0.0F);
                }
            }

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
