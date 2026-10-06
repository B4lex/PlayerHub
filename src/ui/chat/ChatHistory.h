#pragma once

#include "protocol/ChatMessage.h"
#include "sync/chat/ChatSync.h"
#include "ui/Window.h"
#include "ui/chat/ChatPlayerMenu.h"

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace UI
{
    class ChatHistory
    {
    public:
        void Collect(double a_now, bool a_open);

        void SetShown(Multiplayer::ChatChannel a_channel, bool a_shown);
        void ScrollToLatest() { scrollToLatest = true; }

        [[nodiscard]] static float GetLinePitch();

        void DrawFilters();

        std::optional<Multiplayer::RosterEntry> DrawLines(
            const WindowInput& a_input, ImVec2 a_size, bool a_closeMenu, bool a_wheelElsewhere);
        [[nodiscard]] bool IsMenuOpen() const { return menuOpen; }

        float FindRecent(double a_now);
        void DrawRecent(double a_now) const;

    private:
        struct Line
        {
            ChatSync::LineKind kind{ ChatSync::LineKind::kPlayer };
            Multiplayer::ChatChannel channel{ Multiplayer::ChatChannel::kGlobal };
            std::string label;
            std::string text;
            Multiplayer::RosterEntry player;
            Multiplayer::SessionID sender{ 0 };
            bool muted{ false };
            double receivedAt{ 0.0 };
        };

        static constexpr std::size_t kMaximumRecentLines = 6;

        [[nodiscard]] bool Shows(Multiplayer::ChatChannel a_channel) const;
        [[nodiscard]] bool Shows(const Line& a_line) const;
        void Evict(std::size_t a_kind);

        std::deque<Line> lines;
        std::array<std::size_t, 4> counts{};
        std::vector<ChatSync::ChatLine> incoming;

        std::array<bool, 3> shown{ true, true, true };
        std::array<bool, 3> unread{};

        std::uint64_t filtersRevision{ 0 };

        bool scrollToLatest{ false };
        bool arrived{ false };
        ImVec2 shownSize;
        float shownWidth{ 0.0F };
        bool following{ true };
        // Dear ImGui would also count how far from their place the lines were drawn.
        float rewrappedHeight{ -1.0F };

        ChatPlayerMenu playerMenu;
        bool menuOpen{ false };

        std::array<const Line*, kMaximumRecentLines> recent{};
        std::size_t recentCount{ 0 };
    };
}
