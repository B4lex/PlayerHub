#include "ui/chat/ChatWindow.h"
#include "sync/chat/PlayerActivation.h"
#include "sync/notices/Notices.h"
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
#include <string>
#include <utility>
#include <vector>

namespace UI
{
    namespace
    {
        // The bottom margin keeps the chat above the vanilla magicka bar.
        constexpr ImVec2 kMarginFonts{ 0.75F, 5.5F };
        constexpr float kPlayerListGapFonts = 0.5F;

        constexpr float kOpenBackgroundAlpha = 0.6F;
        constexpr float kManagerBackgroundAlpha = 0.9F;
        constexpr float kRecentBackgroundAlpha = 0.25F;

        constexpr ImGuiWindowFlags kWindowFlags
            = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
        constexpr ImGuiWindowFlags kOpenWindowFlags
            = kWindowFlags | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoBringToFrontOnFocus;
        constexpr ImGuiWindowFlags kManagerWindowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings
            | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        constexpr ImGuiWindowFlags kRecentWindowFlags = kWindowFlags | ImGuiWindowFlags_NoInputs
            | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize;

        float GetFramePadding()
        {
            const auto frame = MenuFrame::GetMetrics();
            return std::floor((frame.inset + frame.corner) * 0.5F);
        }

        float GetSidePadding()
        {
            return (ImGui::GetStyle().WindowPadding.x + GetFramePadding()) * 2.0F;
        }

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

        float GetPlayerListWidth()
        {
            return ChatPlayers::GetWidth() + GetSidePadding();
        }

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
            [[nodiscard]] bool HidesIdleCursor() const override { return true; }
            [[nodiscard]] std::optional<ScreenPoint> GetCursorStart() const override { return cursorStart; }
            [[nodiscard]] bool UsesTab() const override { return true; }

            void Draw(const WindowInput& a_input) override
            {
                if (const auto player = PlayerActivation::TakeWhisperRequest();
                    player && (a_input.focused || WindowManager::Focus(*this)))
                {
                    Whisper(*player);
                }

                const auto now = ImGui::GetTime();
                history.Collect(now, a_input.focused);

                if (a_input.focused && !wasFocused)
                {
                    focusInput = true;
                    history.ScrollToLatest();
                    input.OnOpened();
                }
                wasFocused = a_input.focused;
                RouteRejections(a_input.focused);

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
                // Inside where the open chat's frame would be, so the lines stay in their
                // column when it opens.
                SetNextChatWindowPosition(GetFramePadding());
                ImGui::SetNextWindowSizeConstraints(ImVec2{ width, 0.0F }, ImVec2{ width, FLT_MAX });
                ImGui::SetNextWindowBgAlpha(kRecentBackgroundAlpha * alpha);
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
                const auto listWidth = players.IsOpen() ? GetPlayerListGap() + GetPlayerListWidth() : 0.0F;
                const auto units = GetUnits(ImVec2{ sidePadding + listWidth, aroundHeight });
                size.Follow(units);
                const auto linesSize = size.Get(units);
                SetNextChatWindowPosition(0.0F);
                // Both given, so the window has the size in the frame a drag
                // moves it. Sized to its content, it would follow a frame late in
                // height and never get narrower in width.
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

                const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
                const bool menuOpen
                    = history.IsMenuOpen() || input.IsMenuOpen() || players.IsMenuOpen() || manager.IsMenuOpen();
                const bool closeMenu = escape && menuOpen;
                const bool closeManager = escape && !menuOpen && manager.IsOpen();

                auto& composer = input.GetComposer();
                if (!(composer.GetTarget() == shownTarget))
                {
                    shownTarget = composer.GetTarget();
                    history.SetShown(shownTarget.channel, true);
                }
                const auto ruleGap = padding.y + framePadding - MenuFrame::GetMetrics().inset;
                history.DrawFilters();
                players.DrawButton();
                ChatStyle::DrawRule(ruleGap);

                const auto inputRowHeight = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeight();
                aroundHeight = ImGui::GetCursorScreenPos().y - ImGui::GetWindowPos().y + inputRowHeight + padding.y
                    + framePadding;
                const ImVec2 grip{ padding.x + framePadding, padding.y + framePadding };
                size.Grab(ChatSize::Grip{ .depth = grip,
                              .corner = (std::max)({ MenuFrame::GetMetrics().corner, grip.x, grip.y }) },
                    units);
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
                    DrawPlayerManager(closeMenu);
                }
            }

            void DrawPlayerManager(bool a_closeMenu)
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
                    if (const auto player = manager.Draw(a_closeMenu))
                    {
                        Whisper(*player);
                    }
                }
                ImGui::End();
            }

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

            // A rejection arrives after Enter; with the chat closed by then, the field is gone.
            void RouteRejections(bool a_open)
            {
                ChatSync::DrainRejections(rejections);
                for (auto& rejection : rejections)
                {
                    if (a_open)
                    {
                        input.GetComposer().Fail(std::move(rejection));
                    }
                    else
                    {
                        Notices::Post(std::move(rejection));
                    }
                }
                rejections.clear();
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
            float aroundHeight{ 0.0F };
            ChatSync::ChatTarget shownTarget;
            std::vector<std::string> rejections;
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
