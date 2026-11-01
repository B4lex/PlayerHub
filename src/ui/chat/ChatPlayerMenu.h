#pragma once

#include "protocol/ChatMessage.h"

#include <cstdint>
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
        void DrawVoiceVolume();

        Multiplayer::PlayerIdentity player;
        // The slider's value while it is dragged; saved on release.
        int voiceVolume{ 0 };
        bool voiceVolumeActive{ false };
        std::string details;
        float waited{ 0.0F };
        bool pending{ false };
        bool inTradeRange{ false };
    };
}
