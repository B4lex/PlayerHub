#pragma once

#include "protocol/ChatMessage.h"

#include <string>

namespace UI
{
    class ChatPlayerMenu
    {
    public:
        void Open(const Multiplayer::RosterEntry& a_player);

        [[nodiscard]] const Multiplayer::RosterEntry& GetPlayer() const { return player; }

        [[nodiscard]] bool Draw();

    private:
        void DrawHeading();
        void DrawFilterItems() const;

        Multiplayer::RosterEntry player;
        std::string details;
        float waited{ 0.0F };
        bool pending{ false };
    };
}
