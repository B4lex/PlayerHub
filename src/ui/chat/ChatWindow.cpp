#include "ui/chat/ChatWindow.h"
#include "ui/Layout.h"
#include "ui/MenuFrame.h"
#include "ui/Theme.h"
#include "ui/WindowManager.h"
#include "ui/chat/ChatHistory.h"
#include "ui/chat/ChatInput.h"

#include <imgui.h>

#include <cfloat>
#include <cmath>
#include <cstdint>
#include <optional>

namespace UI
{
    namespace
    {
        // In font sizes, so the layout follows the overlay scale. The bottom margin
        // keeps the chat above the vanilla magicka bar.
        constexpr float kWidthFonts = 30.0F;
        constexpr float kHistoryLines = 10.0F;
        constexpr ImVec2 kMarginFonts{ 0.75F, 5.5F };

        constexpr float kOpenBackgroundAlpha = 0.6F;
        constexpr float kRecentBackgroundAlpha = 0.25F;

        constexpr ImGuiWindowFlags kWindowFlags
            = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
        // MenuFrame draws the open chat's background.
        constexpr ImGuiWindowFlags kOpenWindowFlags
            = kWindowFlags | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoBackground;
        constexpr ImGuiWindowFlags kRecentWindowFlags = kWindowFlags | ImGuiWindowFlags_NoInputs
            | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize;

        // What the open chat's frame takes on each side. A knot cuts across
        // its corner, so halfway into it is clear.
        float GetFramePadding()
        {
            const auto frame = MenuFrame::GetMetrics();
            return std::floor((frame.inset + frame.corner) * 0.5F);
        }

        // a_inset in from the chat's corner on both axes.
        void SetNextChatWindowPosition(float a_inset)
        {
            const auto fontSize = ImGui::GetFontSize();
            SetNextWindowCorner(ScreenCorner::kBottomLeft,
                ImVec2{ kMarginFonts.x * fontSize + a_inset, kMarginFonts.y * fontSize + a_inset });
        }

        class ChatWindow final : public Window
        {
        public:
            [[nodiscard]] bool IsFocusKey(std::uint32_t a_scanCode) const override
            {
                return a_scanCode == RE::BSKeyboardDevice::Key::kEnter
                    || a_scanCode == RE::BSKeyboardDevice::Key::kKP_Enter;
            }

            [[nodiscard]] bool UsesMouse() const override { return true; }
            // Most of the time the chat is opened to type a line and closed again.
            [[nodiscard]] bool HidesIdleCursor() const override { return true; }
            [[nodiscard]] std::optional<ScreenPoint> GetCursorStart() const override { return cursorStart; }
            [[nodiscard]] bool UsesTab() const override { return true; }

            void Draw(const WindowInput& a_input) override
            {
                const auto now = ImGui::GetTime();
                history.Collect(now, a_input.focused);

                if (a_input.focused && !wasFocused)
                {
                    focusInput = true;
                    history.ScrollToLatest();
                    input.OnOpened();
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
            void DrawRecent(double a_now)
            {
                const auto alpha = history.FindRecent(a_now);
                if (alpha <= 0.0F)
                {
                    return;
                }

                const Theme::Scope theme;
                const auto width = kWidthFonts * ImGui::GetFontSize();
                // Inside where the open chat's frame would be, so the lines
                // stay in their column when it opens.
                SetNextChatWindowPosition(GetFramePadding());
                ImGui::SetNextWindowSizeConstraints(ImVec2{ width, 0.0F }, ImVec2{ width, FLT_MAX });
                ImGui::SetNextWindowBgAlpha(kRecentBackgroundAlpha * alpha);
                // The theme's border is for windows the player acts on.
                ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
                const bool visible = ImGui::Begin("##ChatRecent", nullptr, kRecentWindowFlags);
                ImGui::PopStyleVar();
                if (visible)
                {
                    history.DrawRecent(a_now);
                }
                ImGui::End();
            }

            void DrawOpen(const WindowInput& a_input)
            {
                const Theme::Scope theme;
                SetNextChatWindowPosition(0.0F);
                if (focusInput)
                {
                    ImGui::SetNextWindowFocus();
                }
                const auto framePadding = GetFramePadding();
                const auto& padding = ImGui::GetStyle().WindowPadding;
                ImGui::PushStyleVar(
                    ImGuiStyleVar_WindowPadding, ImVec2{ padding.x + framePadding, padding.y + framePadding });
                const bool visible = ImGui::Begin("##Chat", nullptr, kOpenWindowFlags);
                ImGui::PopStyleVar();
                if (!visible)
                {
                    ImGui::End();
                    return;
                }
                auto background = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
                background.w = kOpenBackgroundAlpha;
                MenuFrame::Draw(ImGui::GetColorU32(Theme::GetFrameColor()), ImGui::GetColorU32(background));

                // An open menu takes Escape before the name list and the chat do.
                const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
                const bool menuOpen = history.IsMenuOpen() || input.IsMenuOpen();
                const bool closeMenu = escape && menuOpen;

                auto& composer = input.GetComposer();
                // Changed since the previous frame by a key, a prefix, or a click.
                if (!(composer.GetTarget() == shownTarget))
                {
                    shownTarget = composer.GetTarget();
                    history.SetShown(shownTarget.channel, true);
                }
                // As far from the rule under them as from the frame's line above.
                history.DrawFilters(padding.y + framePadding - MenuFrame::GetMetrics().inset);

                const ImVec2 historySize{ kWidthFonts * ImGui::GetFontSize(),
                    kHistoryLines * ChatHistory::GetLinePitch() };
                if (const auto player = history.DrawLines(a_input, historySize, closeMenu))
                {
                    composer.SetTarget(
                        ChatSync::ChatTarget{ .channel = Multiplayer::ChatChannel::kWhisper, .player = *player });
                }

                input.DrawListing();
                const auto outcome = input.Draw(ChatInput::Frame{
                    .focus = focusInput, .escape = escape && !menuOpen, .closeMenu = closeMenu, .menuOpen = menuOpen });
                focusInput = false;
                if (outcome != ChatInput::Outcome::kNone)
                {
                    history.ScrollToLatest();
                }
                if (outcome == ChatInput::Outcome::kClose)
                {
                    WindowManager::ReleaseFocus(*this);
                }

                const auto position = ImGui::GetWindowPos();
                const auto size = ImGui::GetWindowSize();
                cursorStart = ScreenPoint{ position.x + size.x * 0.5F, position.y + size.y * 0.5F };
                ImGui::End();
            }

            ChatHistory history;
            ChatInput input;
            // The target whose channel was last turned on.
            ChatSync::ChatTarget shownTarget;
            // The middle of the open chat as it was last drawn.
            std::optional<ScreenPoint> cursorStart;
            bool wasFocused{ false };
            bool focusInput{ false };
        };
    }

    std::unique_ptr<Window> CreateChatWindow()
    {
        return std::make_unique<ChatWindow>();
    }
}
