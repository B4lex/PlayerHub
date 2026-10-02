#pragma once

#include "protocol/ChatMessage.h"
#include "sync/chat/ChatSync.h"
#include "ui/Window.h"

#include <imgui.h>

#include <array>
#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace UI
{
    // The chat's lines: what arrived, which of them the channel filters show,
    // and how they are drawn in the open chat and, briefly, in the closed one.
    class ChatHistory
    {
    public:
        // Takes the lines that arrived since the previous call. a_open is
        // whether the open chat shows them, which decides what is unread.
        void Collect(double a_now, bool a_open);

        // A channel turned off shows in neither chat. Notices always show.
        void SetShown(Multiplayer::ChatChannel a_channel, bool a_shown);
        void ScrollToLatest() { scrollToLatest = true; }

        // From one line of the open chat to the next, at the current font size.
        [[nodiscard]] static float GetLinePitch();

        // The filter row of the open chat: a toggle per channel, and a rule
        // a_ruleGap under them.
        void DrawFilters(float a_ruleGap);

        // The lines of the open chat with their menu, which a_closeMenu
        // closes; a_size as a child window takes it. Returns a player picked
        // to whisper.
        std::optional<Multiplayer::RosterEntry> DrawLines(const WindowInput& a_input, ImVec2 a_size, bool a_closeMenu);
        [[nodiscard]] bool IsMenuOpen() const { return menuOpen; }

        // Finds the lines the closed chat still shows and returns how opaque
        // the newest is; zero for none. DrawRecent draws what it found.
        float FindRecent(double a_now);
        void DrawRecent(double a_now) const;

    private:
        struct Line
        {
            ChatSync::LineKind kind{ ChatSync::LineKind::kPlayer };
            Multiplayer::ChatChannel channel{ Multiplayer::ChatChannel::kGlobal };
            // The sender and a colon; empty for notices.
            std::string label;
            std::string text;
            // Whom a click on the label whispers; no slot when nobody.
            Multiplayer::RosterEntry player;
            double receivedAt{ 0.0 };
        };

        static constexpr std::size_t kMaximumRecentLines = 6;

        [[nodiscard]] bool Shows(Multiplayer::ChatChannel a_channel) const;
        [[nodiscard]] bool Shows(const Line& a_line) const;
        void Evict(std::size_t a_kind);

        std::deque<Line> lines;
        // Lines held per channel, then notices: a busy channel only pushes
        // out its own lines.
        std::array<std::size_t, 4> counts{};
        // Reused so draining an empty queue allocates nothing.
        std::vector<ChatSync::ChatLine> incoming;

        // By channel: whether its lines show.
        std::array<bool, 3> shown{ true, true, true };
        // By channel: lines arrived that the open chat has not shown.
        std::array<bool, 3> unread{};

        bool scrollToLatest{ false };
        bool arrived{ false };
        // Of the open chat's lines as last drawn: their size, and whether the
        // latest was in view.
        ImVec2 shownSize;
        bool following{ true };
        // What the lines of a resized chat took, for the next frame: Dear
        // ImGui would also count how far they were drawn from their place.
        // Negative when there is nothing to pass on.
        float resizedHeight{ -1.0F };

        Line menuLine;
        bool menuOpen{ false };

        std::array<const Line*, kMaximumRecentLines> recent{};
        std::size_t recentCount{ 0 };
    };
}
