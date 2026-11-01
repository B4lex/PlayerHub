#pragma once

#include "protocol/Session.h"

#include <cstdint>

#include <string>
#include <string_view>
#include <vector>

namespace Network::PlayerFilterStore
{
    inline constexpr std::uint32_t kDefaultVoiceVolume = 100;
    inline constexpr std::uint32_t kMaximumVoiceVolume = 200;

    struct Entry
    {
        Multiplayer::SessionID session{ 0 };
        std::string name;
        bool friended{ false };
        bool muted{ false };
        bool vanished{ false };
        // Percent of the player's voice volume, 0 to kMaximumVoiceVolume.
        std::uint32_t voiceVolume{ kDefaultVoiceVolume };
    };

    void Copy(std::string_view a_server, std::vector<Entry>& a_entries);

    void CopyVanished(std::string_view a_server, std::vector<Multiplayer::SessionID>& a_sessions);

    bool Save(std::string_view a_server, const Entry& a_entry);
}
