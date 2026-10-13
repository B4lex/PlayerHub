#include "ui/chat/ChatWindow.h"
#include "config/PluginConfig.h"
#include "sync/chat/PlayerActivation.h"
#include "sync/notices/Notices.h"
#include "ui/MenuFrame.h"
#include "ui/Theme.h"
#include "ui/WindowManager.h"
#include "ui/chat/ChatHistory.h"
#include "ui/chat/ChatInput.h"
#include "ui/chat/ChatPlayerManager.h"
#include "ui/chat/ChatPlayers.h"
#include "ui/chat/ChatPosition.h"
#include "ui/chat/ChatSize.h"
#include "ui/chat/ChatStyle.h"

#include <imgui.h>

#include <algorithm>
#include <atomic>
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
        constexpr float kPlayerListGapFonts = 0.5F;

        // So a chat dragged against an edge, whose room is exactly its size,
        // keeps that size through the division by the step.
        constexpr float kRoomTolerancePixels = 0.5F;

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

        ImVec2 GetStep()
        {
            return ImVec2{ ImGui::GetFontSize(), ChatHistory::GetLinePitch() };
        }

        // The room from the anchor to the right and top edges.
        ChatSize::Units GetUnits(const ImVec2& a_anchor, const ImVec2& a_around)
        {
            const auto& display = ImGui::GetIO().DisplaySize;
            const auto step = GetStep();
            const auto width = display.x - a_anchor.x - a_around.x + kRoomTolerancePixels;
            const auto height = a_anchor.y - a_around.y + kRoomTolerancePixels;
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

        // Moves the anchor only for a frame at its minimum size, which the room
        // does not hold.
        ImVec2 KeepOnScreen(const ImVec2& a_anchor, const ImVec2& a_frameSize)
        {
            const auto& display = ImGui::GetIO().DisplaySize;
            return ImVec2{ std::floor(std::clamp(a_anchor.x, 0.0F, (std::max)(display.x - a_frameSize.x, 0.0F))),
                std::floor(std::clamp(a_anchor.y, (std::min)(a_frameSize.y, display.y), display.y)) };
        }

        void SetNextChatWindowPosition(const ImVec2& a_corner, float a_inset)
        {
            ImGui::SetNextWindowPos(
                ImVec2{ a_corner.x + a_inset, a_corner.y - a_inset }, ImGuiCond_Always, ImVec2{ 0.0F, 1.0F });
        }

        class ChatWindow final : public Window
        {
        public:
            [[nodiscard]] bool IsFocusKey(std::uint32_t a_scanCode) const override
            {
                const auto& controls = Config::Get()->controls;
                return a_scanCode != 0 && (a_scanCode == controls.chatKey || a_scanCode == controls.playersKey);
            }

            void OnFocusKey(std::uint32_t a_scanCode) override
            {
                managerOnly.store(a_scanCode != Config::Get()->controls.chatKey, std::memory_order_release);
            }

            [[nodiscard]] bool UsesMouse() const override { return true; }
            [[nodiscard]] bool HidesIdleCursor() const override { return !IsManagerOnly(); }
            [[nodiscard]] std::optional<ScreenPoint> GetCursorStart() const override
            {
                return IsManagerOnly() ? std::nullopt : cursorStart;
            }
            [[nodiscard]] bool UsesTab() const override { return true; }

            void Draw(const WindowInput& a_input) override
            {
                if (const auto player = PlayerActivation::TakeWhisperRequest();
                    player && (a_input.focused || WindowManager::Focus(*this)))
                {
                    if (IsManagerOnly())
                    {
                        manager.Close();
                    }
                    managerOnly.store(false, std::memory_order_release);
                    Whisper(*player);
                }

                const bool managerShown = a_input.focused && IsManagerOnly();
                if (managerShown && !wasFocused)
                {
                    manager.Open();
                }
                const bool open = a_input.focused && !managerShown;

                const auto now = ImGui::GetTime();
                history.Collect(now, open);

                if (open && !wasOpen)
                {
                    focusInput = true;
                    history.ScrollToLatest();
                    input.OnOpened();
                }
                wasFocused = a_input.focused;
                wasOpen = open;
                RouteRejections(open);

                if (open)
                {
                    DrawOpen(a_input);
                    return;
                }
                size.Release();
                position.Release();
                if (managerShown)
                {
                    DrawManagerOnly();
                }
                else
                {
                    // A menu that opens by itself closes the chat without Enter or Escape.
                    manager.Close();
                }
                DrawRecent(now);
            }

        private:
            enum class ListSide : std::uint8_t
            {
                kRight,
                kLeft
            };

            [[nodiscard]] bool IsManagerOnly() const { return managerOnly.load(std::memory_order_acquire); }

            [[nodiscard]] ImVec2 GetConfiguredFrameSize() const
            {
                const auto lines = size.Get(ChatSize::Units{ .step = GetStep(),
                    .maximum = { .width = Config::UISettings::kMaximumChatWidth,
                        .lines = Config::UISettings::kMaximumChatLines } });
                return ImVec2{ lines.x + GetSidePadding(), lines.y + aroundHeight };
            }

            [[nodiscard]] ImVec2 GetClosedFrameSize(const ImVec2& a_anchor) const
            {
                const auto sidePadding = GetSidePadding();
                const auto lines = size.Get(GetUnits(a_anchor, ImVec2{ sidePadding, aroundHeight }));
                return ImVec2{ lines.x + sidePadding, lines.y + aroundHeight };
            }

            void DrawRecent(double a_now)
            {
                const auto alpha = history.FindRecent(a_now);
                if (alpha <= 0.0F)
                {
                    return;
                }

                const Theme::Scope theme;
                const auto anchor = position.Get();
                const auto frameSize = GetClosedFrameSize(anchor);
                const auto width = frameSize.x - GetSidePadding();
                // Inside where the open chat's frame would be, so the lines stay in their
                // column when it opens.
                SetNextChatWindowPosition(KeepOnScreen(anchor, frameSize), GetFramePadding());
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
                position.Follow(GetConfiguredFrameSize());
                const auto anchor = position.Get();
                auto units = GetUnits(anchor, ImVec2{ sidePadding, aroundHeight });
                const auto listWidth = players.IsOpen() ? GetPlayerListGap() + GetPlayerListWidth() : 0.0F;
                const auto listSide = players.IsOpen()
                    ? ChooseListSide(anchor.x, size.Get(units).x + sidePadding, listWidth)
                    : ListSide::kRight;
                if (players.IsOpen() && listSide == ListSide::kRight)
                {
                    units = GetUnits(anchor, ImVec2{ sidePadding + listWidth, aroundHeight });
                }
                size.Follow(units);
                const auto linesSize = size.Get(units);
                const ImVec2 frameSize{ linesSize.x + sidePadding, linesSize.y + aroundHeight };
                auto corner = KeepOnScreen(anchor, frameSize);
                if (listSide == ListSide::kRight)
                {
                    corner.x = std::floor((std::min)(
                        corner.x, (std::max)(ImGui::GetIO().DisplaySize.x - frameSize.x - listWidth, 0.0F)));
                }
                SetNextChatWindowPosition(corner, 0.0F);
                // Both given, so the window has the size in the frame a drag
                // moves it. Sized to its content, it would follow a frame late in
                // height and never get narrower in width.
                ImGui::SetNextWindowSize(ImVec2{ frameSize.x, aroundHeight > 0.0F ? frameSize.y : 0.0F });
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
                const bool resizing = !position.IsHeld()
                    && size.Grab(ChatSize::Grip{ .depth = grip,
                                     .corner = (std::max)({ MenuFrame::GetMetrics().corner, grip.x, grip.y }) },
                        units);
                const auto picked = history.DrawLines(
                    a_input, ImVec2{ 0.0F, -inputRowHeight }, closeMenu, players.IsHovered() || manager.IsHovered());
                if (picked)
                {
                    Whisper(*picked);
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
                // After every item, so Dear ImGui knows whether one is under the cursor.
                // A name takes its click without being an item.
                if (!resizing && !picked && !menuOpen)
                {
                    position.Grab(corner);
                }

                const auto windowPos = ImGui::GetWindowPos();
                const auto windowSize = ImGui::GetWindowSize();
                cursorStart = ScreenPoint{ windowPos.x + windowSize.x * 0.5F, windowPos.y + windowSize.y * 0.5F };
                ImGui::End();

                if (players.IsOpen())
                {
                    DrawPlayerList(windowPos, windowSize, listSide, ruleGap, closeMenu);
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

            void DrawManagerOnly()
            {
                const Theme::Scope theme;
                const bool escape = ImGui::IsKeyPressed(ImGuiKey_Escape, false);
                if (escape && !manager.IsMenuOpen())
                {
                    manager.Close();
                }
                else
                {
                    DrawPlayerManager(escape);
                }
                // Closed, but not by a whisper, which goes on in the chat.
                if (!manager.IsOpen() && IsManagerOnly())
                {
                    WindowManager::ReleaseFocus(*this);
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
                        if (IsManagerOnly())
                        {
                            manager.Close();
                            managerOnly.store(false, std::memory_order_release);
                        }
                        Whisper(*player);
                    }
                }
                ImGui::End();
            }

            // Decided from the configured size rather than the one the list leaves, so
            // the side does not change as the chat narrows for the list.
            static ListSide ChooseListSide(float a_left, float a_frameWidth, float a_listWidth)
            {
                return a_left + a_frameWidth + a_listWidth <= ImGui::GetIO().DisplaySize.x || a_left < a_listWidth
                    ? ListSide::kRight
                    : ListSide::kLeft;
            }

            void DrawPlayerList(const ImVec2& a_chatPos, const ImVec2& a_chatSize, ListSide a_side, float a_ruleGap,
                bool a_closeMenu)
            {
                const auto gap = GetPlayerListGap();
                const auto width = GetPlayerListWidth();
                ImGui::SetNextWindowPos(ImVec2{
                    a_side == ListSide::kLeft ? a_chatPos.x - gap - width : a_chatPos.x + a_chatSize.x + gap,
                    a_chatPos.y });
                ImGui::SetNextWindowSize(ImVec2{ width, a_chatSize.y });
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
            ChatPosition position;
            float aroundHeight{ 0.0F };
            ChatSync::ChatTarget shownTarget;
            std::vector<std::string> rejections;
            std::optional<ScreenPoint> cursorStart;
            // Written on the input thread before a focus key gives the window focus,
            // and on this one only while the window holds it.
            std::atomic_bool managerOnly{ false };
            bool wasFocused{ false };
            bool wasOpen{ false };
            bool focusInput{ false };
        };
    }

    std::unique_ptr<Window> CreateChatWindow()
    {
        return std::make_unique<ChatWindow>();
    }
}
