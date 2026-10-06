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
    class ChatPlayers
    {
    public:
        void DrawButton();

        [[nodiscard]] bool IsOpen() const { return open; }
        void Close();

        [[nodiscard]] static float GetWidth();

        struct ListOutcome
        {
            std::optional<Multiplayer::RosterEntry> whisper;
            bool manage{ false };
        };

        ListOutcome DrawList(float a_ruleGap, bool a_closeMenu);

        [[nodiscard]] bool IsMenuOpen() const { return menuOpen; }
        [[nodiscard]] bool IsHovered() const;

    private:
        void Refresh(double a_now);
        void Match();
        void DrawRows(std::optional<Multiplayer::RosterEntry>& a_picked, bool a_closeMenu);

        bool open{ false };

        std::uint64_t revision{ 0 };
        std::vector<ChatRoster::ListedPlayer> players;
        Multiplayer::PeerSlot localPlayer{ 0 };
        std::vector<std::uint32_t> matches;
        std::size_t nearbyMatches{ 0 };
        std::vector<Multiplayer::PeerSlot> nearby;
        std::vector<Multiplayer::PeerSlot> previousNearby;
        double nearbyAt{ -1.0e9 };
        std::optional<std::uint64_t> filtersRevision;
        std::vector<Multiplayer::SessionID> muted;
        std::vector<Multiplayer::SessionID> vanished;

        std::array<char, Multiplayer::PlayerAppearance::kMaximumNameLength + 1> search{};
        std::string foldedSearch;

        std::size_t labelCount{ SIZE_MAX };
        std::array<char, 16> label{};

        Multiplayer::RosterEntry menuPlayer;
        bool menuOpen{ false };

        ImVec2 shownMin;
        ImVec2 shownMax;
    };
}
