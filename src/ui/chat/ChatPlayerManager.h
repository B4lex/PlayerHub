#pragma once

#include "protocol/AppearanceState.h"
#include "protocol/Session.h"
#include "sync/chat/ChatRoster.h"
#include "sync/filters/PlayerFilters.h"
#include "ui/chat/ChatPlayerMenu.h"

#include <imgui.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace UI
{
    class ChatPlayerManager
    {
    public:
        void Open();
        void Close();
        [[nodiscard]] bool IsOpen() const { return open; }
        [[nodiscard]] bool IsHovered() const;
        [[nodiscard]] bool IsMenuOpen() const { return menuOpen; }
        [[nodiscard]] bool TakeFocusRequest() { return std::exchange(focusRequested, false); }

        [[nodiscard]] static ImVec2 GetSize();

        [[nodiscard]] std::optional<Multiplayer::PlayerIdentity> Draw(bool a_closeMenu);

    private:
        enum class Tab : std::uint8_t
        {
            kFriends,
            kMuted,
            kVanished
        };

        struct Row
        {
            Multiplayer::PlayerIdentity player;
            std::string folded;
            bool online{ false };
        };

        void Refresh();
        void Match();
        void DrawTabs();
        void DrawRows(std::optional<Multiplayer::PlayerIdentity>& a_whisper, bool a_closeMenu);

        bool open{ false };
        bool focusRequested{ false };
        Tab tab{ Tab::kFriends };

        std::optional<std::uint64_t> filtersRevision;
        std::uint64_t rosterRevision{ 0 };
        std::vector<ChatRoster::ListedPlayer> roster;
        Multiplayer::PeerSlot localPlayer{ 0 };
        std::vector<std::pair<Multiplayer::SessionID, std::uint32_t>> online;
        std::vector<PlayerFilters::Entry> entries;
        bool stale{ true };

        std::vector<Row> rows;
        std::vector<std::uint32_t> matches;

        std::array<char, Multiplayer::PlayerAppearance::kMaximumNameLength + 1> search{};
        std::string foldedSearch;

        ChatPlayerMenu playerMenu;
        bool menuOpen{ false };

        ImVec2 shownMin;
        ImVec2 shownMax;
    };
}
