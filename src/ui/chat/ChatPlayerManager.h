#pragma once

#include "protocol/AppearanceState.h"
#include "protocol/Session.h"
#include "sync/chat/ChatRoster.h"
#include "sync/filters/PlayerFilters.h"

#include <imgui.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace UI
{
    // What the Manage players window shows, opened from the player list over
    // the open chat: tabs of friends, muted, and vanished players of the
    // current server, offline ones included, where a player is unmuted or
    // brought back. ChatWindow places and begins its window.
    class ChatPlayerManager
    {
    public:
        // Opens it, or brings it to the front when it is open.
        void Open();
        // Forgets the search and the tab: it opens on Friends again.
        void Close();
        [[nodiscard]] bool IsOpen() const { return open; }
        // Whether the cursor is over the window as it was last drawn.
        [[nodiscard]] bool IsHovered() const;
        // Whether the window should take focus this frame, once per Open.
        [[nodiscard]] bool TakeFocusRequest() { return std::exchange(focusRequested, false); }

        // Of the window's content, in pixels.
        [[nodiscard]] static ImVec2 GetSize();

        // In the current window: a title with a close button, the tabs, a
        // search field, and the current tab's players.
        void Draw();

    private:
        enum class Tab : std::uint8_t
        {
            kFriends,
            kMuted,
            kVanished
        };

        struct Row
        {
            Multiplayer::RosterEntry player;
            std::string folded;
            bool online{ false };
        };

        void Refresh();
        void Match();
        void DrawTabs();
        void DrawRows();

        bool open{ false };
        bool focusRequested{ false };
        Tab tab{ Tab::kFriends };

        // Copied when they change while the window shows.
        std::optional<std::uint64_t> filtersRevision;
        std::uint64_t rosterRevision{ 0 };
        std::vector<ChatRoster::ListedPlayer> roster;
        Multiplayer::PeerSlot localPlayer{ 0 };
        // The roster's sessions with their index in it, sorted by session.
        std::vector<std::pair<Multiplayer::SessionID, std::uint32_t>> online;
        std::vector<PlayerFilters::Entry> entries;
        bool stale{ true };

        // The current tab's players, sorted by folded name.
        std::vector<Row> rows;
        // Indices into rows that the search matches.
        std::vector<std::uint32_t> matches;

        std::array<char, Multiplayer::PlayerAppearance::kMaximumNameLength + 1> search{};
        std::string foldedSearch;

        ImVec2 shownMin;
        ImVec2 shownMax;
    };
}
