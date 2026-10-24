#pragma once

#include "protocol/ChatMessage.h"

#include <string>

namespace UI
{
    class ChatPlayerMenu
    {
    public:
        void Open(const Multiplayer::PlayerIdentity& a_player);

        [[nodiscard]] const Multiplayer::PlayerIdentity& GetPlayer() const { return player; }

        [[nodiscard]] bool Draw();

    private:
        void DrawHeading();
        void DrawTradeItems() const;
        void DrawFilterItems() const;

        Multiplayer::PlayerIdentity player;
        std::string details;
        float waited{ 0.0F };
        bool pending{ false };
        bool inTradeRange{ false };
    };
}
