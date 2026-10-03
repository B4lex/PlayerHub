#pragma once

#include "protocol/AppearanceState.h"
#include "protocol/ChatMessage.h"
#include "sync/chat/ChatRoster.h"

#include <imgui.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace UI
{
    // The players online: a button at the end of the open chat's filter row
    // with their number, and the list it opens beside the chat, where a click
    // on a name whispers the player as a click on a sender's name does,
    // marks show who is muted or vanished, and a button at the bottom opens
    // Manage players.
    class ChatPlayers
    {
    public:
        // At the right end of the row the previous item is on.
        void DrawButton();

        // Whether the list shows beside the open chat. It stays open while the
        // chat is closed and shows again when it opens.
        [[nodiscard]] bool IsOpen() const { return open; }
        void Close();

        // Of the list's content, in pixels.
        [[nodiscard]] static float GetWidth();

        struct ListOutcome
        {
            // A player picked to whisper.
            std::optional<Multiplayer::RosterEntry> whisper;
            // Manage players was pressed.
            bool manage{ false };
        };

        // The list in the current window: a search field, a rule a_ruleGap
        // under it, the names, with a menu that a_closeMenu closes, and the
        // Manage players button at the bottom.
        ListOutcome DrawList(float a_ruleGap, bool a_closeMenu);

        [[nodiscard]] bool IsMenuOpen() const { return menuOpen; }
        // Whether the cursor is over the list as it was last drawn.
        [[nodiscard]] bool IsHovered() const;

    private:
        void Refresh(double a_now);
        void Match();
        void DrawRows(std::optional<Multiplayer::RosterEntry>& a_picked, bool a_closeMenu);

        bool open{ false };

        // A copy of the roster, taken when it changes while the list shows.
        std::uint64_t revision{ 0 };
        std::vector<ChatRoster::ListedPlayer> players;
        Multiplayer::PeerSlot localPlayer{ 0 };
        // Indices into players other than the local one that the search
        // matches: the first nearbyMatches nearby, then the others.
        std::vector<std::uint32_t> matches;
        std::size_t nearbyMatches{ 0 };
        // Sorted, refreshed every kNearbySeconds while the list shows.
        std::vector<Multiplayer::PeerSlot> nearby;
        std::vector<Multiplayer::PeerSlot> previousNearby;
        double nearbyAt{ -1.0e9 };
        // Sorted, copied when PlayerFilters changes while the list shows.
        std::optional<std::uint64_t> filtersRevision;
        std::vector<Multiplayer::SessionID> muted;
        std::vector<Multiplayer::SessionID> vanished;

        std::array<char, Multiplayer::PlayerAppearance::kMaximumNameLength + 1> search{};
        std::string foldedSearch;

        // The button's label, rebuilt when the number changes.
        std::size_t labelCount{ SIZE_MAX };
        std::array<char, 16> label{};

        Multiplayer::RosterEntry menuPlayer;
        bool menuOpen{ false };

        ImVec2 shownMin;
        ImVec2 shownMax;
    };
}
