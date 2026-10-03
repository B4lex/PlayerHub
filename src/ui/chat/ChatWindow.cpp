#include "ui/chat/ChatWindow.h"
#include "ui/Layout.h"
#include "ui/MenuFrame.h"
#include "ui/Theme.h"
#include "ui/WindowManager.h"
#include "ui/chat/ChatHistory.h"
#include "ui/chat/ChatInput.h"
#include "ui/chat/ChatPlayerManager.h"
#include "ui/chat/ChatPlayers.h"
#include "ui/chat/ChatSize.h"
#include "ui/chat/ChatStyle.h"

#include <imgui.h>

#include <algorithm>
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
        constexpr ImVec2 kMarginFonts{ 0.75F, 5.5F };
        // In font sizes, between the open chat and the player list.
        constexpr float kPlayerListGapFonts = 0.5F;

        constexpr float kOpenBackgroundAlpha = 0.6F;
        // Over the chat and the game, Manage players needs to stand out.
        constexpr float kManagerBackgroundAlpha = 0.9F;
        constexpr float kRecentBackgroundAlpha = 0.25F;

        constexpr ImGuiWindowFlags kWindowFlags
            = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
        // MenuFrame draws the open chat's background. A click in the chat or
        // the player list leaves Manage players in front of them.
        constexpr ImGuiWindowFlags kOpenWindowFlags
            = kWindowFlags | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus;
        // Manage players is dragged by any empty part.
        constexpr ImGuiWindowFlags kManagerWindowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        constexpr ImGuiWindowFlags kRecentWindowFlags = kWindowFlags | ImGuiWindowFlags_NoInputs
            | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize;

        // What the open chat's frame takes on each side. A knot cuts across
        // its corner, so halfway into it is clear.
        float GetFramePadding()
        {
            const auto frame = MenuFrame::GetMetrics();
            return std::floor((frame.inset + frame.corner) * 0.5F);
        }

        // What the open chat has around its lines on both sides.
        float GetSidePadding()
        {
            return (ImGui::GetStyle().WindowPadding.x + GetFramePadding()) * 2.0F;
        }

        // What the chat's size is measured in, and the lines the screen has
        // room for with the chat's margin from every edge and a_around of the
        // open chat around them.
        ChatSize::Units GetUnits(const ImVec2& a_around)
        {
            const auto& display = ImGui::GetIO().DisplaySize;
            const auto fontSize = ImGui::GetFontSize();
            const ImVec2 step{ fontSize, ChatHistory::GetLinePitch() };
            const auto width = display.x - kMarginFonts.x * fontSize * 2.0F - a_around.x;
            const auto height = display.y - (kMarginFonts.x + kMarginFonts.y) * fontSize - a_around.y;
            return ChatSize::Units{ .step = step,
                .maximum = { .width = static_cast<std::uint32_t>((std::max)(width / step.x, 0.0F)),
                    .lines = static_cast<std::uint32_t>((std::max)(height / step.y, 0.0F)) } };
        }

        float GetPlayerListGap()
        {
            return std::floor(kPlayerListGapFonts * ImGui::GetFontSize());
        }

        // The player list's window, with what is around its content.
        float GetPlayerListWidth()
        {
            return ChatPlayers::GetWidth() + GetSidePadding();
        }

        // A window of the open chat: inside the menu frame, which keeps the
        // content clear of its knots, over the chat's background.
        bool BeginFramed(const char* a_name, ImGuiWindowFlags a_flags = kOpenWindowFlags,
            float a_backgroundAlpha = kOpenBackgroundAlpha)
        {
            const auto framePadding = GetFramePadding();
            const auto& padding = ImGui::GetStyle().WindowPadding;
            ImGui::PushStyleVar(
                ImGuiStyleVar_WindowPadding, ImVec2{ padding.x + framePadding, padding.y + framePadding });
            const bool visible = ImGui::Begin(a_name, nullptr, a_flags);
            ImGui::PopStyleVar();
            if (visible)
            {
                auto background = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
                background.w = a_backgroundAlpha;
                MenuFrame::Draw(ImGui::GetColorU32(Theme::GetFrameColor()), ImGui::GetColorU32(background));
            }
            return visible;
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
                    size.Release();
                    // A menu that opens by itself closes the chat without Enter or Escape.
                    manager.Close();
                    DrawRecent(now);
                }
            }

        private:
            // Of the lines, in pixels: as wide as configured or dragged, and no
            // wider than the screen has room for.
            [[nodiscard]] float GetLinesWidth() const { return size.Get(GetUnits(ImVec2{ GetSidePadding(), 0.0F })).x; }

            void DrawRecent(double a_now)
            {
                const auto alpha = history.FindRecent(a_now);
                if (alpha <= 0.0F)
                {
                    return;
                }

                const Theme::Scope theme;
                const auto width = GetLinesWidth();
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
                const auto sidePadding = GetSidePadding();
                // The player list takes its share of the screen's width.
                const auto listWidth = players.IsOpen() ? GetPlayerListGap() + GetPlayerListWidth() : 0.0F;
                const auto units = GetUnits(ImVec2{ sidePadding + listWidth, aroundHeight });
                size.Follow(units);
                const auto linesSize = size.Get(units);
                SetNextChatWindowPosition(0.0F);
                // Both given, so the window has the size in the frame a drag
                // moves it. As high as its content it would follow a frame
                // late, with the input row off its place meanwhile; as wide as
                // its content it would never get narrower, since the rule and
                // the input row take the width there is. Only a new window is
                // as high as its content, before aroundHeight is measured, and
                // Dear ImGui hides it for that frame.
                ImGui::SetNextWindowSize(
                    ImVec2{ linesSize.x + sidePadding, aroundHeight > 0.0F ? linesSize.y + aroundHeight : 0.0F });
                if (focusInput)
                {
                    ImGui::SetNextWindowFocus();
                }
                if (!BeginFramed("##Chat"))
                {
                    ImGui::End();
                    return;
                }
                const auto framePadding = GetFramePadding();
                const auto& padding = ImGui::GetStyle().WindowPadding;

                // An open menu takes Escape first, then Manage players, then
                // the name list and the chat.
                const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
                const bool menuOpen = history.IsMenuOpen() || input.IsMenuOpen() || players.IsMenuOpen();
                const bool closeMenu = escape && menuOpen;
                const bool closeManager = escape && !menuOpen && manager.IsOpen();

                auto& composer = input.GetComposer();
                // Changed since the previous frame by a key, a prefix, or a click.
                if (!(composer.GetTarget() == shownTarget))
                {
                    shownTarget = composer.GetTarget();
                    history.SetShown(shownTarget.channel, true);
                }
                // As far from the rule under them as from the frame's line above.
                const auto ruleGap = padding.y + framePadding - MenuFrame::GetMetrics().inset;
                history.DrawFilters();
                players.DrawButton();
                ChatStyle::DrawRule(ruleGap);

                const auto inputRowHeight = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeight();
                aroundHeight = ImGui::GetCursorScreenPos().y - ImGui::GetWindowPos().y + inputRowHeight + padding.y
                    + framePadding;
                // The strips the frame runs in, which hold nothing to click.
                const ImVec2 grip{ padding.x + framePadding, padding.y + framePadding };
                size.Grab(ChatSize::Grip{ .depth = grip,
                              .corner = (std::max)({ MenuFrame::GetMetrics().corner, grip.x, grip.y }) },
                    units);
                // The lines take what the window leaves above the input row.
                if (const auto player = history.DrawLines(
                        a_input, ImVec2{ 0.0F, -inputRowHeight }, closeMenu, players.IsHovered() || manager.IsHovered()))
                {
                    Whisper(*player);
                }

                input.DrawListing();
                const auto outcome = input.Draw(ChatInput::Frame{ .focus = focusInput,
                    .escape = escape && !menuOpen && !closeManager,
                    .closeMenu = closeMenu,
                    .menuOpen = menuOpen });
                focusInput = false;
                if (outcome != ChatInput::Outcome::kNone)
                {
                    history.ScrollToLatest();
                }
                if (outcome == ChatInput::Outcome::kClose)
                {
                    WindowManager::ReleaseFocus(*this);
                    manager.Close();
                }

                const auto windowPos = ImGui::GetWindowPos();
                const auto windowSize = ImGui::GetWindowSize();
                cursorStart = ScreenPoint{ windowPos.x + windowSize.x * 0.5F, windowPos.y + windowSize.y * 0.5F };
                ImGui::End();

                if (players.IsOpen())
                {
                    DrawPlayerList(windowPos, windowSize, ruleGap, closeMenu);
                }
                if (closeManager)
                {
                    manager.Close();
                }
                if (manager.IsOpen())
                {
                    DrawPlayerManager();
                }
            }

            // In the middle of the screen when it opens, then where the
            // player drags it, in front of the chat.
            void DrawPlayerManager()
            {
                const auto& display = ImGui::GetIO().DisplaySize;
                const auto content = ChatPlayerManager::GetSize();
                const auto around = GetSidePadding();
                ImGui::SetNextWindowPos(
                    ImVec2{ display.x * 0.5F, display.y * 0.5F }, ImGuiCond_Appearing, ImVec2{ 0.5F, 0.5F });
                ImGui::SetNextWindowSize(ImVec2{ content.x + around, content.y + around });
                if (manager.TakeFocusRequest())
                {
                    ImGui::SetNextWindowFocus();
                }
                if (BeginFramed("##ChatPlayerManager", kManagerWindowFlags, kManagerBackgroundAlpha))
                {
                    manager.Draw();
                }
                ImGui::End();
            }

            // Right of the open chat and as high, so its rule lines up with
            // the chat's.
            void DrawPlayerList(const ImVec2& a_chatPos, const ImVec2& a_chatSize, float a_ruleGap, bool a_closeMenu)
            {
                ImGui::SetNextWindowPos(ImVec2{ a_chatPos.x + a_chatSize.x + GetPlayerListGap(), a_chatPos.y });
                ImGui::SetNextWindowSize(ImVec2{ GetPlayerListWidth(), a_chatSize.y });
                if (BeginFramed("##ChatPlayers"))
                {
                    const auto outcome = players.DrawList(a_ruleGap, a_closeMenu);
                    if (outcome.whisper)
                    {
                        Whisper(*outcome.whisper);
                    }
                    if (outcome.manage)
                    {
                        manager.Open();
                    }
                }
                ImGui::End();
            }

            void Whisper(const Multiplayer::RosterEntry& a_player)
            {
                input.GetComposer().SetTarget(
                    ChatSync::ChatTarget{ .channel = Multiplayer::ChatChannel::kWhisper, .player = a_player });
            }

            ChatHistory history;
            ChatInput input;
            ChatPlayers players;
            ChatPlayerManager manager;
            ChatSize size;
            // Of the open chat as last drawn, around its lines: the filters
            // above and the input row below, with the padding. Zero before it
            // is drawn.
            float aroundHeight{ 0.0F };
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
