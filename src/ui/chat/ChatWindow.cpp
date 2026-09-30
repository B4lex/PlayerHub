#include "ui/chat/ChatWindow.h"
#include "protocol/ChatMessage.h"
#include "sync/chat/ChatSync.h"
#include "ui/Layout.h"
#include "ui/WindowManager.h"

#include <imgui.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace UI
{
    namespace
    {
        constexpr std::size_t kMaximumHistoryLines = 100;
        constexpr std::size_t kMaximumRecentLines = 6;
        constexpr double kRecentLineSeconds = 10.0;
        constexpr double kRecentFadeSeconds = 2.0;
        constexpr float kWheelLines = 3.0F;

        // In font sizes, so the layout follows the overlay scale. The bottom margin
        // keeps the chat above the vanilla magicka bar.
        constexpr float kWidthFonts = 30.0F;
        constexpr float kHistoryLines = 10.0F;
        constexpr ImVec2 kMarginFonts{ 0.75F, 5.5F };

        constexpr float kOpenBackgroundAlpha = 0.6F;
        constexpr float kRecentBackgroundAlpha = 0.25F;

        constexpr ImVec4 kGlobalSenderColor{ 0.95F, 0.80F, 0.45F, 1.0F };
        constexpr ImVec4 kLocalSenderColor{ 0.60F, 0.95F, 0.35F, 1.0F };
        constexpr ImVec4 kWhisperSenderColor{ 0.78F, 0.56F, 1.0F, 1.0F };
        constexpr ImVec4 kTextColor{ 0.92F, 0.92F, 0.92F, 1.0F };
        constexpr ImVec4 kNoticeColor{ 0.95F, 0.55F, 0.40F, 1.0F };

        constexpr ImGuiWindowFlags kWindowFlags
            = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
        constexpr ImGuiWindowFlags kRecentWindowFlags = kWindowFlags | ImGuiWindowFlags_NoInputs
            | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize;

        struct HistoryLine
        {
            ChatSync::LineKind kind{ ChatSync::LineKind::kPlayer };
            Multiplayer::ChatChannel channel{ Multiplayer::ChatChannel::kGlobal };
            // The sender and a colon; empty for notices.
            std::string label;
            std::string text;
            double receivedAt{ 0.0 };
        };

        ImVec4 WithAlpha(ImVec4 a_color, float a_alpha)
        {
            a_color.w *= a_alpha;
            return a_color;
        }

        float GetRecentAlpha(const HistoryLine& a_line, double a_now)
        {
            const auto remaining = kRecentLineSeconds - (a_now - a_line.receivedAt);
            return static_cast<float>(std::clamp(remaining / kRecentFadeSeconds, 0.0, 1.0));
        }

        const ImVec4& GetSenderColor(Multiplayer::ChatChannel a_channel)
        {
            switch (a_channel)
            {
                case Multiplayer::ChatChannel::kLocal:
                    return kLocalSenderColor;
                case Multiplayer::ChatChannel::kWhisper:
                    return kWhisperSenderColor;
                case Multiplayer::ChatChannel::kGlobal:
                    break;
            }
            return kGlobalSenderColor;
        }

        void DrawLine(const HistoryLine& a_line, float a_alpha)
        {
            if (a_line.kind == ChatSync::LineKind::kPlayer)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(GetSenderColor(a_line.channel), a_alpha));
                ImGui::TextUnformatted(a_line.label.c_str(), a_line.label.c_str() + a_line.label.size());
                ImGui::PopStyleColor();
                ImGui::SameLine();
            }

            const auto& textColor = a_line.kind == ChatSync::LineKind::kPlayer ? kTextColor : kNoticeColor;
            ImGui::PushStyleColor(ImGuiCol_Text, WithAlpha(textColor, a_alpha));
            ImGui::PushTextWrapPos(0.0F);
            ImGui::TextUnformatted(a_line.text.c_str(), a_line.text.c_str() + a_line.text.size());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }

        void SetNextChatWindowPosition()
        {
            const auto fontSize = ImGui::GetFontSize();
            SetNextWindowCorner(
                ScreenCorner::kBottomLeft, ImVec2{ kMarginFonts.x * fontSize, kMarginFonts.y * fontSize });
        }

        class ChatWindow final : public Window
        {
        public:
            [[nodiscard]] bool IsFocusKey(std::uint32_t a_scanCode) const override
            {
                return a_scanCode == RE::BSKeyboardDevice::Key::kEnter
                    || a_scanCode == RE::BSKeyboardDevice::Key::kKP_Enter;
            }

            void Draw(const WindowInput& a_input) override
            {
                const auto now = ImGui::GetTime();
                CollectLines(now);

                if (a_input.focused && !wasFocused)
                {
                    focusInput = true;
                    scrollToLatest = true;
                }
                wasFocused = a_input.focused;

                if (a_input.focused)
                {
                    DrawOpen(a_input);
                }
                else
                {
                    DrawRecent(now);
                }
            }

        private:
            void CollectLines(double a_now)
            {
                ChatSync::DrainLines(incoming);
                for (auto& line : incoming)
                {
                    if (history.size() >= kMaximumHistoryLines)
                    {
                        history.pop_front();
                    }
                    const bool fromPlayer = line.kind == ChatSync::LineKind::kPlayer;
                    history.push_back(HistoryLine{ .kind = line.kind,
                        .channel = line.channel,
                        .label = fromPlayer ? std::move(line.sender) + ":" : std::string{},
                        .text = std::move(line.text),
                        .receivedAt = a_now });
                    scrollToLatest = scrollToLatest || followingLatest;
                }
                incoming.clear();
            }

            void DrawRecent(double a_now) const
            {
                std::size_t visible = 0;
                for (auto line = history.rbegin(); line != history.rend() && visible < kMaximumRecentLines; ++line)
                {
                    if (a_now - line->receivedAt >= kRecentLineSeconds)
                    {
                        break;
                    }
                    ++visible;
                }
                if (visible == 0)
                {
                    return;
                }

                const auto width = kWidthFonts * ImGui::GetFontSize();
                SetNextChatWindowPosition();
                ImGui::SetNextWindowSizeConstraints(ImVec2{ width, 0.0F }, ImVec2{ width, FLT_MAX });
                ImGui::SetNextWindowBgAlpha(kRecentBackgroundAlpha * GetRecentAlpha(history.back(), a_now));
                if (ImGui::Begin("##ChatRecent", nullptr, kRecentWindowFlags))
                {
                    const auto first = history.end() - static_cast<std::ptrdiff_t>(visible);
                    for (auto line = first; line != history.end(); ++line)
                    {
                        DrawLine(*line, GetRecentAlpha(*line, a_now));
                    }
                }
                ImGui::End();
            }

            void DrawOpen(const WindowInput& a_input)
            {
                SetNextChatWindowPosition();
                ImGui::SetNextWindowBgAlpha(kOpenBackgroundAlpha);
                if (focusInput)
                {
                    ImGui::SetNextWindowFocus();
                }
                if (!ImGui::Begin("##Chat", nullptr, kWindowFlags | ImGuiWindowFlags_AlwaysAutoResize))
                {
                    ImGui::End();
                    return;
                }

                const ImVec2 historySize{ kWidthFonts * ImGui::GetFontSize(),
                    kHistoryLines * ImGui::GetTextLineHeightWithSpacing() };
                DrawHistory(a_input, historySize);

                // Keyboard focus applies on a later frame, so it is requested until taken.
                if (focusInput || !inputActive)
                {
                    ImGui::SetKeyboardFocusHere();
                }
                focusInput = false;
                ImGui::SetNextItemWidth(-FLT_MIN);
                const bool submitted = ImGui::InputText(
                    "##ChatInput", draft.data(), draft.size(), ImGuiInputTextFlags_EnterReturnsTrue);
                inputActive = ImGui::IsItemActive();

                if (submitted)
                {
                    // Accepted messages come back from the server like everyone else's.
                    ChatSync::SendLocalMessage(draft.data());
                    draft[0] = '\0';
                    followingLatest = true;
                    scrollToLatest = true;
                    WindowManager::ReleaseFocus(*this);
                }
                else if (ImGui::IsItemDeactivated())
                {
                    // Escape reverts the draft and deactivates the input.
                    WindowManager::ReleaseFocus(*this);
                }
                ImGui::End();
            }

            void DrawHistory(const WindowInput& a_input, ImVec2 a_size)
            {
                if (ImGui::BeginChild("##ChatHistory", a_size))
                {
                    for (const auto& line : history)
                    {
                        DrawLine(line, 1.0F);
                    }

                    const auto lineHeight = ImGui::GetTextLineHeightWithSpacing();
                    auto scroll = ImGui::GetScrollY() - a_input.wheelSteps * kWheelLines * lineHeight;
                    if (ImGui::IsKeyPressed(ImGuiKey_PageUp))
                    {
                        scroll -= a_size.y - lineHeight;
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_PageDown))
                    {
                        scroll += a_size.y - lineHeight;
                    }
                    if (scroll != ImGui::GetScrollY())
                    {
                        ImGui::SetScrollY(std::clamp(scroll, 0.0F, ImGui::GetScrollMaxY()));
                        followingLatest = scroll >= ImGui::GetScrollMaxY();
                    }
                    if (scrollToLatest)
                    {
                        ImGui::SetScrollHereY(1.0F);
                        scrollToLatest = false;
                        followingLatest = true;
                    }
                }
                ImGui::EndChild();
            }

            std::deque<HistoryLine> history;
            // Reused so draining an empty queue allocates nothing.
            std::vector<ChatSync::ChatLine> incoming;
            std::array<char, Multiplayer::kMaximumChatTextBytes + 1> draft{};
            bool wasFocused{ false };
            bool focusInput{ false };
            bool inputActive{ false };
            // Whether new lines scroll the history down; false while scrolled back.
            bool followingLatest{ true };
            bool scrollToLatest{ false };
        };
    }

    std::unique_ptr<Window> CreateChatWindow()
    {
        return std::make_unique<ChatWindow>();
    }
}
